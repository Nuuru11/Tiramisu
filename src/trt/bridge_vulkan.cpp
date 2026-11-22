// tiramisu/src/trt/bridge_vulkan.cpp
// Vulkan 1.0-1.3 + WSI ICD -> SPIR-V->HLSL -> D3D11 bridge.
#include "bridge_vulkan.h"
#include "input.h"
#include "spirv_hlsl.h"
#include "thunk.h"

// Real Vulkan ABI for the physical-device properties/limits structs: hand-
// rolled mirrors drifted once (a zeroed limits block reported
// maxFragmentInputComponents=0 -> Godot computed "maximum varyings = 0" ->
// EVERY shader compile failed -> endless scene-import retry storm). The
// header layout is stable and pointer-free; the static_asserts below pin it.
#if __has_include(<vulkan/vulkan_core.h>)
#include <vulkan/vulkan_core.h>
#else
#include "vulkan_core_stub.h"
#endif

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

// Settings knob (shader_cache): weak C-linkage wrapper the cache checks.
// The real definition (settings.cpp) returns the bool getter as int; weak
// missing resolves to 0 handled by check_enabled()'s default-on logic.
extern "C" int trt_settings_shader_cache_int(void) __attribute__((weak));

// ---- shader compile cache (settings knob: shader_cache) -------------------
// SPIR-V -> HLSL compilation is the single most expensive path on the Vulkan
// surface (Godot compiles every shader variant at scene load; measured: an
// end less retry storm when compiles failed). When the knob is on, compile
// results are memoized by FNV-1a hash of the SPIR-V words + stage + entry +
// spec constants. In-process memoization only — the console has no writable
// shader disk cache under AppContainer without a mounted surface.
#include <mutex>
#include <unordered_map>
namespace vkshader_cache {
struct cache_key {
  u64 hash;
  u32 stage;
  u64 spec_hash;
  bool operator==(const cache_key& o) const {
    return hash == o.hash && stage == o.stage && spec_hash == o.spec_hash;
  }
};
struct cache_key_h {
  size_t operator()(const cache_key& k) const {
    return (size_t)(k.hash * 0x100000001b3ull ^ (u64)k.stage ^
                    (k.spec_hash << 13));
  }
};
static std::mutex g_mu;
static std::unordered_map<cache_key, spirv_compile_result, cache_key_h> g_cache;
static bool g_enabled = true;   // default matches settings default
static bool g_checked = false;

static void check_enabled() {
  if (g_checked) return;
  g_checked = true;
  // Weak-missing symbol resolves to nullptr at link time (same pattern as
  // trt_settings_write_watch in bridge_kernel32): call only when linked.
  g_enabled = true;  // default on
  if (trt_settings_shader_cache_int) g_enabled = (trt_settings_shader_cache_int() != 0);}

static u64 fnv1a(const u32* w, u32 n) {
  u64 h = 0xcbf29ce484222325ull;
  for (u32 i = 0; i < n; ++i) {
    h ^= (u64)w[i];
    h *= 0x100000001b3ull;
  }
  return h;
}
static u64 spec_fnv(const spirv_spec_entry* s, u32 n) {
  u64 h = 0x9e3779b97f4a7c15ull;
  for (u32 i = 0; i < n; ++i) {
    h ^= (u64)s[i].spec_id ^ ((u64)s[i].value_u32 << 8);
    h *= 0x100000001b3ull;
  }
  return h;
}
}  // namespace vkshader_cache

// Memoized compile: returns nullptr when the knob is off or on cache miss
// (caller compiles + stores through spirv_cache_store).
static const spirv_compile_result* spirv_cache_lookup(const u32* words, u32 wc,
                                                      spirv_stage st, const char* name,
                                                      const spirv_spec_entry* specs,
                                                      u32 spec_n) {
  using namespace vkshader_cache;
  check_enabled();
  if (!g_enabled) return nullptr;
  cache_key k{fnv1a(words, wc), (u32)st,
              spec_fnv(specs, spec_n) ^ (u64)(uintptr_t)name};
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(k);
  return it == g_cache.end() ? nullptr : &it->second;
}
static void spirv_cache_store(const u32* words, u32 wc, spirv_stage st,
                              const char* name, const spirv_spec_entry* specs,
                              u32 spec_n, const spirv_compile_result& cr) {
  using namespace vkshader_cache;
  if (!g_enabled) return;
  cache_key k{fnv1a(words, wc), (u32)st,
              spec_fnv(specs, spec_n) ^ (u64)(uintptr_t)name};
  std::lock_guard<std::mutex> lk(g_mu);
  g_cache.emplace(k, cr);
}
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

static std::mutex g_vk_mu;
static trt_vk_hooks g_vk_hooks = {};
static bool g_vk_hooks_set = false;

static void vk_log(const char* msg) { trt_core_log(msg); }

// Single-line formatted log (trt_core_log takes one line, no trailing \n).
// Newlines in the formatted text are flattened so one call stays one line.
static void vk_logf(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  for (char* p = buf; *p; ++p) {
    if (*p == '\n' || *p == '\r') *p = ' ';
  }
  vk_log(buf);
}

static constexpr i32 VK_SUCCESS_V = 0;
static constexpr i32 VK_NOT_READY_V = 1;
static constexpr i32 VK_EVENT_SET_V = 3;
static constexpr i32 VK_EVENT_RESET_V = 4;
static constexpr i32 VK_INCOMPLETE_V = 5;
static constexpr i32 VK_ERROR_OUT_OF_HOST_MEMORY_V = -1;
static constexpr i32 VK_ERROR_OUT_OF_DEVICE_MEMORY_V = -2;
static constexpr i32 VK_ERROR_MEMORY_MAP_FAILED_V = -8;
static constexpr i32 VK_ERROR_INITIALIZATION_FAILED_V = -3;
static constexpr i32 VK_ERROR_EXTENSION_NOT_PRESENT_V = -7;
static constexpr i32 VK_ERROR_FEATURE_NOT_PRESENT_V = -8;

static constexpr u32 VK_MAKE_VER(u32 maj, u32 min, u32 pat) {
  return (maj << 22) | (min << 12) | pat;
}

// ---- Exact Khronos ABI struct mirrors (Win64 / LP64 natural layout) --------
struct vk_extent2d { u32 width; u32 height; };
struct vk_extent3d { u32 width; u32 height; u32 depth; };
struct vk_offset2d { i32 x; i32 y; };
struct vk_offset3d { i32 x; i32 y; i32 z; };
struct vk_rect2d { vk_offset2d offset; vk_extent2d extent; };
struct vk_viewport { float x, y, width, height, minDepth, maxDepth; };

struct vk_app_info {
  u32 sType; const void* pNext; const char* pApplicationName; u32 applicationVersion;
  const char* pEngineName; u32 engineVersion; u32 apiVersion;
};
struct vk_instance_create_info {
  u32 sType; const void* pNext; u32 flags; const vk_app_info* pApplicationInfo;
  u32 enabledLayerCount; const char* const* ppEnabledLayerNames;
  u32 enabledExtensionCount; const char* const* ppEnabledExtensionNames;
};
struct vk_ext_props {
  char extensionName[256]; u32 specVersion;
};
struct vk_layer_props {
  char layerName[256]; u32 specVersion; u32 implementationVersion; char description[256];
};
// The properties/limits structs now come straight from <vulkan/vulkan_core.h>
// (see the include above). Static asserts pin the ABI the ICD was measured on.
#if __has_include(<vulkan/vulkan_core.h>)
static_assert(sizeof(VkPhysicalDeviceLimits) == 504,
              "VkPhysicalDeviceLimits ABI drift");
static_assert(offsetof(VkPhysicalDeviceProperties, limits) == 296,
              "VkPhysicalDeviceProperties.limits offset drift");
// Feature-struct ABI pins (measured 1.3.275; sizes include trailing padding
// to the 8-byte alignment — the bool counts in feature_struct_bool_count()
// are the real field counts, not (sizeof-16)/4).
static_assert(sizeof(VkPhysicalDeviceFeatures) == 220,
              "VkPhysicalDeviceFeatures ABI drift");
static_assert(sizeof(VkPhysicalDeviceVulkan11Features) == 64,
              "VkPhysicalDeviceVulkan11Features ABI drift");
static_assert(sizeof(VkPhysicalDeviceVulkan12Features) == 208,
              "VkPhysicalDeviceVulkan12Features ABI drift");
static_assert(sizeof(VkPhysicalDeviceVulkan13Features) == 80,
              "VkPhysicalDeviceVulkan13Features ABI drift");
static_assert(sizeof(VkPhysicalDeviceDescriptorIndexingFeatures) == 96,
              "VkPhysicalDeviceDescriptorIndexingFeatures ABI drift");
static_assert(sizeof(VkPhysicalDeviceBufferDeviceAddressFeatures) == 32,
              "VkPhysicalDeviceBufferDeviceAddressFeatures ABI drift");
static_assert(sizeof(VkPhysicalDeviceDynamicRenderingFeatures) == 24,
              "VkPhysicalDeviceDynamicRenderingFeatures ABI drift");
static_assert(sizeof(VkPhysicalDeviceSynchronization2Features) == 24,
              "VkPhysicalDeviceSynchronization2Features ABI drift");
static_assert(sizeof(VkPhysicalDeviceTimelineSemaphoreFeatures) == 24,
              "VkPhysicalDeviceTimelineSemaphoreFeatures ABI drift");
static_assert(offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge) == 16,
              "feature-struct bool offset drift");
#endif
// Local aliases keep the existing call sites unchanged.
using vk_phys_dev_props = VkPhysicalDeviceProperties;
using vk_phys_dev_props2 = VkPhysicalDeviceProperties2;
using vk_phys_dev_limits = VkPhysicalDeviceLimits;
using vk_phys_dev_features = VkPhysicalDeviceFeatures;
using vk_phys_dev_features2 = VkPhysicalDeviceFeatures2;
// props2/features use the real header types (aliased above).
struct vk_qf_props { u32 queueFlags; u32 queueCount; u32 timestampValidBits; vk_extent3d minImageTransferGranularity; };
struct vk_qf_props2 { u32 sType; void* pNext; vk_qf_props queueFamilyProperties; };
struct vk_mem_type { u32 propertyFlags; u32 heapIndex; };
struct vk_mem_heap { u64 size; u32 flags; };
struct vk_mem_props { u32 memoryTypeCount; vk_mem_type memoryTypes[32]; u32 memoryHeapCount; vk_mem_heap memoryHeaps[16]; };
struct vk_mem_props2 { u32 sType; void* pNext; vk_mem_props memoryProperties; };
struct vk_fmt_props { u32 linearTilingFeatures; u32 optimalTilingFeatures; u32 bufferFeatures; };
struct vk_fmt_props2 { u32 sType; void* pNext; vk_fmt_props formatProperties; };

struct vk_dev_queue_ci { u32 sType; const void* pNext; u32 flags; u32 queueFamilyIndex; u32 queueCount; const float* pQueuePriorities; };
struct vk_dev_ci {
  u32 sType; const void* pNext; u32 flags; u32 queueCreateInfoCount;
  const vk_dev_queue_ci* pQueueCreateInfos; u32 enabledLayerCount; const char* const* ppEnabledLayerNames;
  u32 enabledExtensionCount; const char* const* ppEnabledExtensionNames; const vk_phys_dev_features* pEnabledFeatures;
};

struct vk_surface_caps {
  u32 minImageCount; u32 maxImageCount; vk_extent2d currentExtent;
  vk_extent2d minImageExtent; vk_extent2d maxImageExtent; u32 maxImageArrayLayers;
  u32 supportedTransforms; u32 currentTransform; u32 supportedCompositeAlpha; u32 supportedUsageFlags;
};
struct vk_surface_fmt { u32 format; u32 colorSpace; };
struct vk_win32_surface_ci { u32 sType; const void* pNext; u32 flags; void* hinstance; void* hwnd; };
struct vk_swapchain_ci {
  u32 sType; const void* pNext; u32 flags; u64 surface; u32 minImageCount;
  u32 imageFormat; u32 imageColorSpace; vk_extent2d imageExtent; u32 imageArrayLayers;
  u32 imageUsage; u32 imageSharingMode; u32 queueFamilyIndexCount; const u32* pQueueFamilyIndices;
  u32 preTransform; u32 compositeAlpha; u32 presentMode; u32 clipped; u64 oldSwapchain;
};
struct vk_present_info {
  u32 sType; const void* pNext; u32 waitSemaphoreCount; const u64* pWaitSemaphores;
  u32 swapchainCount; const u64* pSwapchains; const u32* pImageIndices; i32* pResults;
};

struct vk_mem_alloc_info { u32 sType; const void* pNext; u64 allocationSize; u32 memoryTypeIndex; };
struct vk_mem_reqs { u64 size; u64 alignment; u32 memoryTypeBits; };
struct vk_mapped_range { u32 sType; const void* pNext; u64 memory; u64 offset; u64 size; };
struct vk_buf_ci {
  u32 sType; const void* pNext; u32 flags; u64 size; u32 usage; u32 sharingMode;
  u32 queueFamilyIndexCount; const u32* pQueueFamilyIndices;
};
struct vk_img_ci {
  u32 sType; const void* pNext; u32 flags; u32 imageType; u32 format;
  vk_extent3d extent; u32 mipLevels; u32 arrayLayers; u32 samples; u32 tiling;
  u32 usage; u32 sharingMode; u32 queueFamilyIndexCount; const u32* pQueueFamilyIndices;
  u32 initialLayout;
};
struct vk_img_subres_layers { u32 aspectMask; u32 mipLevel; u32 baseArrayLayer; u32 layerCount; };
struct vk_img_view_ci {
  u32 sType; const void* pNext; u32 flags; u64 image; u32 viewType; u32 format;
  u32 components[4]; u32 subresRange[5];
};
struct vk_sampler_ci {
  u32 sType; const void* pNext; u32 flags; u32 magFilter; u32 minFilter; u32 mipmapMode;
  u32 addressModeU; u32 addressModeV; u32 addressModeW; float mipLodBias;
  u32 anisotropyEnable; float maxAnisotropy; u32 compareEnable; u32 compareOp;
  float minLod; float maxLod; u32 borderColor; u32 unnormalizedCoordinates;
};
struct vk_shader_mod_ci { u32 sType; const void* pNext; u32 flags; size_t codeSize; const u32* pCode; };
struct vk_spec_map_entry { u32 constantID; u32 offset; size_t size; };
struct vk_spec_info { u32 mapEntryCount; const vk_spec_map_entry* pMapEntries; size_t dataSize; const void* pData; };
struct vk_pipe_shader_stage_ci {
  u32 sType; const void* pNext; u32 flags; u32 stage; u64 module; const char* pName;
  const vk_spec_info* pSpecializationInfo;
};
struct vk_vtx_bind_desc { u32 binding; u32 stride; u32 inputRate; };
struct vk_vtx_attr_desc { u32 location; u32 binding; u32 format; u32 offset; };
struct vk_pipe_vi_ci {
  u32 sType; const void* pNext; u32 flags;
  u32 vertexBindingDescriptionCount; const vk_vtx_bind_desc* pVertexBindingDescriptions;
  u32 vertexAttributeDescriptionCount; const vk_vtx_attr_desc* pVertexAttributeDescriptions;
};
struct vk_pipe_ia_ci { u32 sType; const void* pNext; u32 flags; u32 topology; u32 primitiveRestartEnable; };
struct vk_pipe_vp_ci {
  u32 sType; const void* pNext; u32 flags; u32 viewportCount; const vk_viewport* pViewports;
  u32 scissorCount; const vk_rect2d* pScissors;
};
struct vk_pipe_rs_ci {
  u32 sType; const void* pNext; u32 flags; u32 depthClampEnable; u32 rasterizerDiscardEnable;
  u32 polygonMode; u32 cullMode; u32 frontFace; u32 depthBiasEnable;
  float depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor, lineWidth;
};
struct vk_stencil_op_st { u32 failOp, passOp, depthFailOp, compareOp, compareMask, writeMask, reference; };
struct vk_pipe_ds_ci {
  u32 sType; const void* pNext; u32 flags; u32 depthTestEnable; u32 depthWriteEnable;
  u32 depthCompareOp; u32 depthBoundsTestEnable; u32 stencilTestEnable;
  vk_stencil_op_st front, back; float minDepthBounds, maxDepthBounds;
};
struct vk_pipe_cb_att {
  u32 blendEnable, srcColorBlendFactor, dstColorBlendFactor, colorBlendOp;
  u32 srcAlphaBlendFactor, dstAlphaBlendFactor, alphaBlendOp, colorWriteMask;
};
struct vk_pipe_cb_ci {
  u32 sType; const void* pNext; u32 flags; u32 logicOpEnable; u32 logicOp;
  u32 attachmentCount; const vk_pipe_cb_att* pAttachments; float blendConstants[4];
};
struct vk_gfx_pipe_ci {
  u32 sType; const void* pNext; u32 flags; u32 stageCount;
  const vk_pipe_shader_stage_ci* pStages;
  const vk_pipe_vi_ci* pVertexInputState;
  const vk_pipe_ia_ci* pInputAssemblyState;
  const void* pTessellationState;
  const vk_pipe_vp_ci* pViewportState;
  const vk_pipe_rs_ci* pRasterizationState;
  const void* pMultisampleState;
  const vk_pipe_ds_ci* pDepthStencilState;
  const vk_pipe_cb_ci* pColorBlendState;
  const void* pDynamicState;
  u64 layout; u64 renderPass; u32 subpass; u64 basePipelineHandle; i32 basePipelineIndex;
};
struct vk_comp_pipe_ci {
  u32 sType; const void* pNext; u32 flags; vk_pipe_shader_stage_ci stage;
  u64 layout; u64 basePipelineHandle; i32 basePipelineIndex;
};

struct vk_desc_set_layout_bind { u32 binding, descriptorType, descriptorCount, stageFlags; const u64* pImmutableSamplers; };
struct vk_desc_set_layout_ci { u32 sType; const void* pNext; u32 flags; u32 bindingCount; const vk_desc_set_layout_bind* pBindings; };
struct vk_desc_set_alloc_info { u32 sType; const void* pNext; u64 descriptorPool; u32 descriptorSetCount; const u64* pSetLayouts; };
struct vk_desc_img_info { u64 sampler; u64 imageView; u32 imageLayout; };
struct vk_desc_buf_info { u64 buffer; u64 offset; u64 range; };
struct vk_write_desc_set {
  u32 sType; const void* pNext; u64 dstSet; u32 dstBinding; u32 dstArrayElement;
  u32 descriptorCount; u32 descriptorType; const vk_desc_img_info* pImageInfo;
  const vk_desc_buf_info* pBufferInfo; const u64* pTexelBufferView;
};

struct vk_att_desc {
  u32 flags, format, samples, loadOp, storeOp, stencilLoadOp, stencilStoreOp, initialLayout, finalLayout;
};
struct vk_att_ref { u32 attachment; u32 layout; };
struct vk_subpass_desc {
  u32 flags, pipelineBindPoint, inputAttachmentCount; const vk_att_ref* pInputAttachments;
  u32 colorAttachmentCount; const vk_att_ref* pColorAttachments; const vk_att_ref* pResolveAttachments;
  const vk_att_ref* pDepthStencilAttachment; u32 preserveAttachmentCount; const u32* pPreserveAttachments;
};
struct vk_rp_ci {
  u32 sType; const void* pNext; u32 flags; u32 attachmentCount; const vk_att_desc* pAttachments;
  u32 subpassCount; const vk_subpass_desc* pSubpasses; u32 dependencyCount; const void* pDependencies;
};
struct vk_fb_ci {
  u32 sType; const void* pNext; u32 flags; u64 renderPass;
  u32 attachmentCount; const u64* pAttachments; u32 width, height, layers;
};
struct vk_clear_val { float f[4]; };
struct vk_rp_begin_info {
  u32 sType; const void* pNext; u64 renderPass; u64 framebuffer;
  vk_rect2d renderArea; u32 clearValueCount; const vk_clear_val* pClearValues;
};
struct vk_rendering_att_info {
  u32 sType; const void* pNext; u64 imageView; u32 imageLayout; u32 resolveMode;
  u64 resolveImageView; u32 resolveImageLayout; u32 loadOp; u32 storeOp; vk_clear_val clearValue;
};
struct vk_rendering_info {
  u32 sType; const void* pNext; u32 flags; vk_rect2d renderArea; u32 layerCount; u32 viewMask;
  u32 colorAttachmentCount; const vk_rendering_att_info* pColorAttachments;
  const vk_rendering_att_info* pDepthAttachment; const vk_rendering_att_info* pStencilAttachment;
};
struct vk_cmd_pool_ci { u32 sType; const void* pNext; u32 flags; u32 queueFamilyIndex; };
struct vk_cmd_buf_alloc { u32 sType; const void* pNext; u64 commandPool; u32 level; u32 commandBufferCount; };
struct vk_submit_info {
  u32 sType; const void* pNext; u32 waitSemaphoreCount; const u64* pWaitSemaphores;
  const u32* pWaitDstStageMask; u32 commandBufferCount; void* const* pCommandBuffers;
  u32 signalSemaphoreCount; const u64* pSignalSemaphores;
};
struct vk_buf_copy { u64 srcOffset, dstOffset, size; };
struct vk_buf_img_copy {
  u64 bufferOffset; u32 bufferRowLength; u32 bufferImageHeight;
  vk_img_subres_layers imageSubresource; vk_offset3d imageOffset; vk_extent3d imageExtent;
};

struct vk_img_fmt_props {
  vk_extent3d maxExtent; u32 maxMipLevels; u32 maxArrayLayers; u32 sampleCounts; u64 maxResourceSize;
};
struct vk_phys_dev_img_fmt_info2 {
  u32 sType; const void* pNext; u32 format; u32 type; u32 tiling; u32 usage; u32 flags;
};
struct vk_img_fmt_props2 { u32 sType; void* pNext; vk_img_fmt_props imageFormatProperties; };
struct vk_mem_reqs2 { u32 sType; void* pNext; vk_mem_reqs memoryRequirements; };
struct vk_buf_mem_reqs_info2 { u32 sType; const void* pNext; u64 buffer; };
struct vk_img_mem_reqs_info2 { u32 sType; const void* pNext; u64 image; };
struct vk_bind_buf_mem_info { u32 sType; const void* pNext; u64 buffer; u64 memory; u64 memoryOffset; };
struct vk_bind_img_mem_info { u32 sType; const void* pNext; u64 image; u64 memory; u64 memoryOffset; };
struct vk_img_subres { u32 aspectMask; u32 mipLevel; u32 arrayLayer; };
struct vk_subres_layout { u64 offset; u64 size; u64 rowPitch; u64 arrayPitch; u64 depthPitch; };
struct vk_ext_handle_props {
  u32 sType; void* pNext; u32 exportFromImportedHandleTypes; u32 compatibleHandleTypes; u32 features;
};
struct vk_phys_dev_group_props {
  u32 sType; void* pNext; u32 physicalDeviceCount; void* physicalDevices[32]; u32 subsetAllocation;
};
struct vk_sparse_img_fmt_props { u32 aspectMask; vk_extent3d imageGranularity; u32 flags; };
struct vk_sparse_img_fmt_props2 { u32 sType; void* pNext; vk_sparse_img_fmt_props properties; };
struct vk_sparse_img_mem_reqs {
  vk_sparse_img_fmt_props formatProperties; u32 imageMipTailFirstLod;
  u64 imageMipTailSize; u64 imageMipTailOffset; u64 imageMipTailStride;
};
struct vk_sparse_img_mem_reqs2 { u32 sType; void* pNext; vk_sparse_img_mem_reqs memoryRequirements; };
struct vk_att_desc2 {
  u32 sType; const void* pNext; u32 flags, format, samples, loadOp, storeOp, stencilLoadOp, stencilStoreOp, initialLayout, finalLayout;
};
struct vk_att_ref2 { u32 sType; const void* pNext; u32 attachment, layout, aspectMask; };
struct vk_subpass_desc2 {
  u32 sType; const void* pNext; u32 flags, pipelineBindPoint, viewMask, inputAttachmentCount;
  const vk_att_ref2* pInputAttachments; u32 colorAttachmentCount; const vk_att_ref2* pColorAttachments;
  const vk_att_ref2* pResolveAttachments; const vk_att_ref2* pDepthStencilAttachment;
  u32 preserveAttachmentCount; const u32* pPreserveAttachments;
};
struct vk_rp_ci2 {
  u32 sType; const void* pNext; u32 flags;
  u32 attachmentCount; const vk_att_desc2* pAttachments;
  u32 subpassCount; const vk_subpass_desc2* pSubpasses;
  u32 dependencyCount; const void* pDependencies;
  u32 correlatedViewMaskCount; const u32* pCorrelatedViewMasks;
};
struct vk_pipe_cache_ci { u32 sType; const void* pNext; u32 flags; size_t initialDataSize; const void* pInitialData; };
struct vk_buf_view_ci { u32 sType; const void* pNext; u32 flags; u64 buffer; u32 format; u64 offset; u64 range; };
struct vk_desc_tmpl_entry { u32 dstBinding; u32 dstArrayElement; u32 descriptorCount; u32 descriptorType; size_t offset; size_t stride; };
struct vk_desc_tmpl_ci {
  u32 sType; const void* pNext; u32 flags;
  u32 descriptorUpdateEntryCount; const vk_desc_tmpl_entry* pDescriptorUpdateEntries;
  u32 templateType; u64 descriptorSetLayout; u32 pipelineBindPoint; u64 pipelineLayout; u32 set;
};
struct vk_desc_set_layout_support { u32 sType; void* pNext; u32 supported; };
struct vk_sem_type_ci { u32 sType; const void* pNext; u32 semaphoreType; u64 initialValue; };
struct vk_sem_wait_info { u32 sType; const void* pNext; u32 flags; u32 semaphoreCount; const u64* pSemaphores; const u64* pValues; };
struct vk_sem_signal_info { u32 sType; const void* pNext; u64 semaphore; u64 value; };
struct vk_buf_dev_addr_info { u32 sType; const void* pNext; u64 buffer; };
struct vk_cb_submit_info { u32 sType; const void* pNext; void* commandBuffer; u32 deviceMask; };
struct vk_submit_info2 {
  u32 sType; const void* pNext; u32 flags;
  u32 waitSemaphoreInfoCount; const void* pWaitSemaphoreInfos;
  u32 commandBufferInfoCount; const vk_cb_submit_info* pCommandBufferInfos;
  u32 signalSemaphoreInfoCount; const void* pSignalSemaphoreInfos;
};
struct vk_draw_indirect_cmd { u32 vertexCount, instanceCount, firstVertex, firstInstance; };
struct vk_draw_idx_indirect_cmd { u32 indexCount, instanceCount, firstIndex; i32 vertexOffset; u32 firstInstance; };
struct vk_dispatch_indirect_cmd { u32 x, y, z; };

// ---- Internal ICD Objects --------------------------------------------------
struct vk_mem_obj {
  u64 id = 0;
  u64 size = 0;
  u32 mem_type = 0;
  std::vector<u8> data;
  bool mapped = false;
};
struct vk_buf_obj {
  u64 id = 0;
  u64 size = 0;
  u32 usage = 0;
  u64 mem_id = 0;
  u64 mem_off = 0;
};
struct vk_img_obj {
  u64 id = 0;
  trt_vk_image_desc desc{};
  u64 mem_id = 0;
  u64 mem_off = 0;
};
struct vk_img_view_obj { u64 id = 0; u64 image_id = 0; u32 format = 0; };
struct vk_sampler_obj { u64 id = 0; u32 min_filter = 1; u32 mag_filter = 1; u32 addr_u = 0; u32 addr_v = 0; };
struct vk_shader_obj { u64 id = 0; std::vector<u32> spv; };

struct vk_pipe_binding_map {
  u32 set = 0; u32 binding = 0;
  spirv_binding_kind kind = SPV_BIND_CBV;
  u32 stage_mask = 0x3Fu;
  u32 hlsl_reg = 0; u32 smp_reg = 0;
};
struct vk_pipe_obj {
  u64 id = 0;
  bool is_compute = false;
  u32 push_reg = 13;
  std::vector<vk_pipe_binding_map> bindings;
  vk_viewport init_vp{0, 0, 1280, 720, 0, 1};
  vk_rect2d init_sc{{0, 0}, {1280, 720}};
  bool has_init_vp = false;
  float blend_const[4] = {};
};
struct vk_desc_binding_val {
  u64 buf_id = 0; u64 buf_off = 0; u64 buf_range = 0;
  u64 img_view = 0; u64 sampler = 0;
};
struct vk_desc_set_obj {
  u64 id = 0;
  std::map<u32, vk_desc_binding_val> entries;
};
struct vk_rp_obj {
  u64 id = 0;
  std::vector<u32> att_load_op;
  int depth_att = -1;
  std::vector<u32> color_atts;
};
struct vk_fb_obj {
  u64 id = 0;
  u32 w = 0, h = 0;
  std::vector<u64> view_ids;
};
struct vk_swapchain_obj {
  u64 id = 0;
  u32 w = 1280, h = 720;
  u32 fmt = 44; // B8G8R8A8_UNORM
  std::vector<u64> images;
  u32 next_img = 0;
};

struct vk_cmd_op {
  enum kind_t {
    OP_BEGIN_RP = 1, OP_END_RP = 2, OP_BIND_PIPE = 3, OP_SET_VP = 4, OP_SET_SC = 5,
    OP_BIND_VB = 6, OP_BIND_IB = 7, OP_BIND_DS = 8, OP_PUSH_CONST = 9,
    OP_DRAW = 10, OP_DRAW_IDX = 11, OP_DISPATCH = 12,
    OP_COPY_BUF = 13, OP_COPY_BUF_IMG = 14, OP_BEGIN_RENDERING = 15, OP_END_RENDERING = 16,
    OP_DRAW_INDIRECT = 17, OP_DRAW_IDX_INDIRECT = 18, OP_DRAW_INDIRECT_COUNT = 19,
    OP_DRAW_IDX_INDIRECT_COUNT = 20, OP_DISPATCH_INDIRECT = 21,
    OP_UPDATE_BUF = 22, OP_FILL_BUF = 23, OP_SET_EVENT = 24, OP_RESET_EVENT = 25,
    OP_EXEC_CMDS = 26, OP_PUSH_DS = 27,
  } kind{};
  trt_vk_rp_begin rp{};
  u32 bind_point = 0;
  u64 handle = 0;
  vk_viewport vp{};
  vk_rect2d sc{};
  u32 first_slot = 0, slot_count = 0;
  u64 vbs[8] = {}, vb_offs[8] = {};
  u8 idx_u32 = 0;
  u32 ds_first = 0, ds_count = 0;
  u64 dsets[8] = {};
  // OP_PUSH_DS: snapshot of the pushed writes for set ds_first
  // (binding -> value), resolved at draw time like a bound set.
  std::map<u32, vk_desc_binding_val> push_entries;
  u32 push_off = 0, push_sz = 0;
  u8 push_bytes[256] = {};
  u32 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
  i32 vtx_off = 0;
  u64 dst_handle = 0;
  vk_buf_img_copy bic{};
  std::vector<u8> inline_bytes;
  std::vector<void*> sec_cbs;
};

struct vk_cmdbuf_obj {
  u64 loader_magic = 0x01CDC0DE00000004ull;
  void* device = nullptr;
  bool recording = false;
  std::vector<vk_cmd_op> ops;
};
struct vk_queue_obj {
  u64 loader_magic = 0x01CDC0DE00000003ull;
  void* device = nullptr;
};
struct vk_device_obj {
  u64 loader_magic = 0x01CDC0DE00000002ull;
  vk_queue_obj main_queue{0x01CDC0DE00000003ull, this};
  u64 next_h = 100;
  std::map<u64, vk_mem_obj> mems;
  std::map<u64, vk_buf_obj> bufs;
  std::map<u64, vk_img_obj> imgs;
  std::map<u64, vk_img_view_obj> views;
  std::map<u64, vk_sampler_obj> samps;
  std::map<u64, vk_shader_obj> shaders;
  std::map<u64, vk_pipe_obj> pipes;
  std::map<u64, vk_desc_set_obj> dsets;
  std::map<u64, std::vector<vk_desc_tmpl_entry>> desc_templates;
  std::map<u64, vk_rp_obj> rps;
  std::map<u64, vk_fb_obj> fbs;
  std::map<u64, vk_swapchain_obj> swapchains;
  std::vector<vk_cmdbuf_obj*> cmdbufs;
  std::map<u64, bool> fences;
  std::map<u64, bool> events;
  std::map<u64, u64> semaphores;
  std::map<u64, std::vector<u8>> pipe_caches;
};
struct vk_phys_dev_obj {
  u64 loader_magic = 0x01CDC0DE00000001ull;
  void* instance = nullptr;
};
struct vk_instance_obj {
  u64 loader_magic = 0x01CDC0DE00000000ull;
  vk_phys_dev_obj gpu;
  u32 api_ver = VK_MAKE_VER(1, 3, 255);
};

struct vk_surface_obj { void* hwnd = nullptr; };
static std::map<u64, vk_surface_obj> g_surfaces;
static u64 g_next_surface = 1;
static std::vector<vk_instance_obj*> g_instances;
static std::vector<vk_device_obj*> g_devices;
static vk_device_obj* g_active_dev = nullptr;

static vk_device_obj* dev_from(void* d) {
  return d ? (vk_device_obj*)d : g_active_dev;
}

static void sync_buf_mem(vk_device_obj* d, u64 bid) {
  auto it = d->bufs.find(bid);
  if (it == d->bufs.end()) return;
  const auto& b = it->second;
  const void* ptr = nullptr;
  u64 bytes = b.size;
  if (b.mem_id && d->mems.count(b.mem_id)) {
    const auto& m = d->mems[b.mem_id].data;
    if (b.mem_off < m.size()) {
      ptr = m.data() + b.mem_off;
      bytes = std::min(b.size, (u64)(m.size() - b.mem_off));
    }
  }
  if (g_vk_hooks_set && g_vk_hooks.sync_buffer) {
    g_vk_hooks.sync_buffer(bid, b.usage, ptr, bytes, g_vk_hooks.user);
  }
}

static void sync_img_obj(vk_device_obj* d, const vk_img_obj& im) {
  if (g_vk_hooks_set && g_vk_hooks.sync_image) {
    g_vk_hooks.sync_image(im.id, &im.desc, nullptr, 0, 0, 0, g_vk_hooks.user);
  }
  (void)d;
}

static bool read_buf_mem(vk_device_obj* d, u64 bid, u64 off, void* dst, size_t bytes) {
  if (!d || !dst || !bytes) return false;
  auto bit = d->bufs.find(bid);
  if (bit == d->bufs.end()) return false;
  const auto& b = bit->second;
  if (!b.mem_id || !d->mems.count(b.mem_id)) return false;
  const auto& m = d->mems[b.mem_id].data;
  u64 pos = b.mem_off + off;
  if (off + bytes > b.size || pos + bytes > m.size()) return false;
  memcpy(dst, m.data() + pos, bytes);
  return true;
}

static bool write_buf_mem(vk_device_obj* d, u64 bid, u64 off, const void* src, size_t bytes) {
  if (!d || !src || !bytes) return false;
  auto bit = d->bufs.find(bid);
  if (bit == d->bufs.end()) return false;
  const auto& b = bit->second;
  if (!b.mem_id || !d->mems.count(b.mem_id)) return false;
  auto& m = d->mems[b.mem_id];
  u64 pos = b.mem_off + off;
  if (off >= b.size) return false;
  // Grow-to-need: never allocate the whole block for a small write
  // (a 256MB device-local block used to cost a 256MB host vector).
  u64 need = pos + (u64)bytes;
  if (need > m.size) need = m.size;
  if (m.data.size() < need && need > 0) {
    if (need > (16ull << 20)) {
      char gb[192];
      snprintf(gb, sizeof(gb),
               "[vulkan] write_buf_mem GROW mem=%llu type=%u need=%lluMB (was %zuMB)",
               (unsigned long long)m.id, m.mem_type,
               (unsigned long long)(need >> 20),
               (size_t)(m.data.size() >> 20));
      trt_core_log(gb);
    }
    try {
      m.data.resize((size_t)std::max<u64>(need, 16u));
    } catch (...) {
      return false;
    }
  }
  if (pos >= m.data.size()) return false;
  size_t avail = (size_t)std::min<u64>({(u64)bytes, b.size - off, (u64)(m.data.size() - pos)});
  if (!avail) return false;
  memcpy(m.data.data() + pos, src, avail);
  return true;
}

struct vk_exec_state {
  u64 bound_gfx_pipe = 0, bound_cs_pipe = 0;
  u64 bound_ibs = 0, bound_ib_off = 0;
  u8 idx_u32 = 0;
  u64 bound_vbs[8] = {}, bound_vb_offs[8] = {};
  u32 bound_vb_cnt = 0;
  u64 gfx_dsets[8] = {}, cs_dsets[8] = {};
  // VK_KHR_push_descriptor: per-set pushed contents. A pushed set replaces
  // any bound set at the same index (and vice versa); push_valid tracks
  // which source is live for each set index.
  std::map<u32, vk_desc_binding_val> gfx_push[8], cs_push[8];
  bool gfx_push_valid[8] = {}, cs_push_valid[8] = {};
  u8 push_buf[256] = {};
  u32 push_sz = 0;
  vk_viewport cur_vp{0, 0, 1280, 720, 0, 1};
  vk_rect2d cur_sc{{0, 0}, {1280, 720}};
  bool has_vp = false, has_sc = false;
};

static void exec_cmdbuf_state(vk_device_obj* d, const vk_cmdbuf_obj* cb,
                               vk_exec_state& st, int depth) {
  if (!d || !cb || depth > 8) return;

  auto build_bindings = [&](const vk_pipe_obj& p, const u64* sets,
                            const std::map<u32, vk_desc_binding_val>* push_sets,
                            const bool* push_valid,
                            std::vector<trt_vk_desc_binding>& out) {
    out.clear();
    for (const auto& mb : p.bindings) {
      if (mb.set >= 8) continue;
      // A pushed set replaces the bound set at the same index.
      const vk_desc_binding_val* val = nullptr;
      if (push_valid[mb.set]) {
        auto eit = push_sets[mb.set].find(mb.binding);
        if (eit == push_sets[mb.set].end()) continue;
        val = &eit->second;
      } else {
        if (!sets[mb.set]) continue;
        auto dsit = d->dsets.find(sets[mb.set]);
        if (dsit == d->dsets.end()) continue;
        auto eit = dsit->second.entries.find(mb.binding);
        if (eit == dsit->second.entries.end()) continue;
        val = &eit->second;
      }
      trt_vk_desc_binding db{};
      db.kind = (u32)mb.kind;
      db.stage_mask = mb.stage_mask;
      db.hlsl_reg = mb.hlsl_reg;
      db.smp_reg = mb.smp_reg;
      db.buf_id = val->buf_id;
      db.buf_off = val->buf_off;
      db.buf_range = val->buf_range;
      if (db.buf_id) sync_buf_mem(d, db.buf_id);
      if (val->img_view && d->views.count(val->img_view)) {
        db.img_id = d->views[val->img_view].image_id;
      }
      if (val->sampler && d->samps.count(val->sampler)) {
        const auto& s = d->samps[val->sampler];
        db.min_filter = s.min_filter; db.mag_filter = s.mag_filter;
        db.addr_u = s.addr_u; db.addr_v = s.addr_v;
      }
      out.push_back(db);
    }
  };

  std::vector<trt_vk_desc_binding> tmp_binds;
  auto emit_draw = [&](bool indexed, u32 a0, u32 a1, u32 a2, i32 vtx_off, u32 a3) {
    auto pit = d->pipes.find(st.bound_gfx_pipe);
    if (pit == d->pipes.end()) return;
    build_bindings(pit->second, st.gfx_dsets, st.gfx_push, st.gfx_push_valid, tmp_binds);
    trt_vk_draw_cmd cmd{};
    cmd.pipeline_id = st.bound_gfx_pipe;
    cmd.indexed = indexed ? 1 : 0;
    cmd.idx_is_u32 = st.idx_u32;
    cmd.has_vp = st.has_vp ? 1 : 0;
    cmd.has_sc = st.has_sc ? 1 : 0;
    cmd.ib_id = st.bound_ibs; cmd.ib_off = st.bound_ib_off;
    cmd.vtx_count = a0; cmd.inst_count = a1;
    cmd.first_vtx_or_idx = a2; cmd.vtx_off = vtx_off; cmd.first_inst = a3;
    cmd.vb_count = st.bound_vb_cnt;
    memcpy(cmd.vb_ids, st.bound_vbs, sizeof(cmd.vb_ids));
    memcpy(cmd.vb_offs, st.bound_vb_offs, sizeof(cmd.vb_offs));
    cmd.vp_x = st.cur_vp.x; cmd.vp_y = st.cur_vp.y; cmd.vp_w = st.cur_vp.width;
    cmd.vp_h = st.cur_vp.height; cmd.vp_min_z = st.cur_vp.minDepth; cmd.vp_max_z = st.cur_vp.maxDepth;
    cmd.sc_x = st.cur_sc.offset.x; cmd.sc_y = st.cur_sc.offset.y;
    cmd.sc_w = st.cur_sc.extent.width; cmd.sc_h = st.cur_sc.extent.height;
    memcpy(cmd.blend_const, pit->second.blend_const, 16);
    cmd.push_data = st.push_sz ? st.push_buf : nullptr;
    cmd.push_bytes = st.push_sz;
    cmd.push_reg = pit->second.push_reg;
    cmd.bindings = tmp_binds.empty() ? nullptr : tmp_binds.data();
    cmd.binding_count = (u32)tmp_binds.size();
    if (g_vk_hooks_set && g_vk_hooks.draw) g_vk_hooks.draw(&cmd, g_vk_hooks.user);
    static u64 s_draw_count = 0;
    ++s_draw_count;
    if (s_draw_count <= 5) {
      vk_logf("vulkan: Draw #%llu pipe=%llu idx=%u vtx=%u inst=%u binds=%u",
              (unsigned long long)s_draw_count, (unsigned long long)cmd.pipeline_id,
              (u32)cmd.indexed, cmd.vtx_count, cmd.inst_count, cmd.binding_count);
    }
  };
  auto emit_dispatch = [&](u32 gx, u32 gy, u32 gz) {
    auto pit = d->pipes.find(st.bound_cs_pipe);
    if (pit == d->pipes.end()) return;
    build_bindings(pit->second, st.cs_dsets, st.cs_push, st.cs_push_valid, tmp_binds);
    trt_vk_dispatch_cmd cmd{};
    cmd.pipeline_id = st.bound_cs_pipe;
    cmd.gx = gx; cmd.gy = gy; cmd.gz = gz;
    cmd.push_data = st.push_sz ? st.push_buf : nullptr;
    cmd.push_bytes = st.push_sz;
    cmd.push_reg = pit->second.push_reg;
    cmd.bindings = tmp_binds.empty() ? nullptr : tmp_binds.data();
    cmd.binding_count = (u32)tmp_binds.size();
    if (g_vk_hooks_set && g_vk_hooks.dispatch) g_vk_hooks.dispatch(&cmd, g_vk_hooks.user);
  };

  for (const auto& op : cb->ops) {
    switch (op.kind) {
      case vk_cmd_op::OP_BEGIN_RP:
      case vk_cmd_op::OP_BEGIN_RENDERING: {
        static u64 s_rp_count = 0;
        ++s_rp_count;
        if (s_rp_count <= 5) {
          vk_logf("vulkan: BeginRP #%llu kind=%d color_cnt=%u color0=%llu depth=%llu clear_mask=0x%x area=%ux%u",
                  (unsigned long long)s_rp_count, (int)op.kind, op.rp.color_count,
                  (unsigned long long)op.rp.color_imgs[0], (unsigned long long)op.rp.depth_img,
                  op.rp.clear_color_mask, op.rp.area_w, op.rp.area_h);
        }
        if (g_vk_hooks_set && g_vk_hooks.begin_render_pass) {
          g_vk_hooks.begin_render_pass(&op.rp, g_vk_hooks.user);
        }
      } break;
      case vk_cmd_op::OP_END_RP:
      case vk_cmd_op::OP_END_RENDERING:
        if (g_vk_hooks_set && g_vk_hooks.end_render_pass) {
          g_vk_hooks.end_render_pass(g_vk_hooks.user);
        }
        break;
      case vk_cmd_op::OP_BIND_PIPE:
        if (op.bind_point == 1) st.bound_cs_pipe = op.handle;
        else {
          st.bound_gfx_pipe = op.handle;
          if (!st.has_vp && d->pipes.count(op.handle) && d->pipes[op.handle].has_init_vp) {
            st.cur_vp = d->pipes[op.handle].init_vp; st.has_vp = true;
            st.cur_sc = d->pipes[op.handle].init_sc; st.has_sc = true;
          }
        }
        break;
      case vk_cmd_op::OP_SET_VP: st.cur_vp = op.vp; st.has_vp = true; break;
      case vk_cmd_op::OP_SET_SC: st.cur_sc = op.sc; st.has_sc = true; break;
      case vk_cmd_op::OP_BIND_VB:
        for (u32 i = 0; i < op.slot_count && (op.first_slot + i) < 8u; ++i) {
          st.bound_vbs[op.first_slot + i] = op.vbs[i];
          st.bound_vb_offs[op.first_slot + i] = op.vb_offs[i];
          sync_buf_mem(d, op.vbs[i]);
          st.bound_vb_cnt = std::max(st.bound_vb_cnt, op.first_slot + i + 1u);
        }
        break;
      case vk_cmd_op::OP_BIND_IB:
        st.bound_ibs = op.handle; st.bound_ib_off = op.vb_offs[0]; st.idx_u32 = op.idx_u32;
        sync_buf_mem(d, st.bound_ibs);
        break;
      case vk_cmd_op::OP_BIND_DS: {
        u64* dst = (op.bind_point == 1) ? st.cs_dsets : st.gfx_dsets;
        bool* pv = (op.bind_point == 1) ? st.cs_push_valid : st.gfx_push_valid;
        for (u32 i = 0; i < op.ds_count && (op.ds_first + i) < 8u; ++i) {
          dst[op.ds_first + i] = op.dsets[i];
          pv[op.ds_first + i] = false;  // binding a set replaces any push
        }
      } break;
      case vk_cmd_op::OP_PUSH_DS: {
        if (op.ds_first >= 8u) break;
        auto* push = (op.bind_point == 1) ? st.cs_push : st.gfx_push;
        bool* pv = (op.bind_point == 1) ? st.cs_push_valid : st.gfx_push_valid;
        u64* dst = (op.bind_point == 1) ? st.cs_dsets : st.gfx_dsets;
        // Push updates the bindings it writes (like vkUpdateDescriptorSets
        // on the push set); bindings from earlier pushes survive.
        for (const auto& kv : op.push_entries) {
          push[op.ds_first][kv.first] = kv.second;
        }
        pv[op.ds_first] = true;
        dst[op.ds_first] = 0;  // pushing a set replaces any bound set
      } break;
      case vk_cmd_op::OP_PUSH_CONST:
        if (op.push_off + op.push_sz <= 256u) {
          memcpy(st.push_buf + op.push_off, op.push_bytes, op.push_sz);
          st.push_sz = std::max(st.push_sz, op.push_off + op.push_sz);
        }
        break;
      case vk_cmd_op::OP_DRAW:
        emit_draw(false, op.a0, op.a1, op.a2, op.vtx_off, op.a3);
        break;
      case vk_cmd_op::OP_DRAW_IDX:
        emit_draw(true, op.a0, op.a1, op.a2, op.vtx_off, op.a3);
        break;
      case vk_cmd_op::OP_DRAW_INDIRECT:
      case vk_cmd_op::OP_DRAW_IDX_INDIRECT:
      case vk_cmd_op::OP_DRAW_INDIRECT_COUNT:
      case vk_cmd_op::OP_DRAW_IDX_INDIRECT_COUNT: {
        const bool indexed = (op.kind == vk_cmd_op::OP_DRAW_IDX_INDIRECT ||
                              op.kind == vk_cmd_op::OP_DRAW_IDX_INDIRECT_COUNT);
        const bool is_count = (op.kind == vk_cmd_op::OP_DRAW_INDIRECT_COUNT ||
                               op.kind == vk_cmd_op::OP_DRAW_IDX_INDIRECT_COUNT);
        u32 draw_cnt = op.a0;
        if (is_count) {
          u32 gpu_cnt = 0;
          if (read_buf_mem(d, op.dst_handle, op.vb_offs[1], &gpu_cnt, sizeof(gpu_cnt))) {
            draw_cnt = std::min(draw_cnt, gpu_cnt);
          }
        }
        const u32 def_stride = indexed ? (u32)sizeof(vk_draw_idx_indirect_cmd)
                                       : (u32)sizeof(vk_draw_indirect_cmd);
        const u32 stride = op.a1 ? op.a1 : def_stride;
        for (u32 di = 0; di < draw_cnt; ++di) {
          u64 off = op.vb_offs[0] + (u64)di * stride;
          if (indexed) {
            vk_draw_idx_indirect_cmd c{};
            if (!read_buf_mem(d, op.handle, off, &c, sizeof(c))) break;
            emit_draw(true, c.indexCount, c.instanceCount, c.firstIndex, c.vertexOffset,
                      c.firstInstance);
          } else {
            vk_draw_indirect_cmd c{};
            if (!read_buf_mem(d, op.handle, off, &c, sizeof(c))) break;
            emit_draw(false, c.vertexCount, c.instanceCount, c.firstVertex, 0, c.firstInstance);
          }
        }
      } break;
      case vk_cmd_op::OP_DISPATCH:
        emit_dispatch(op.a0, op.a1, op.a2);
        break;
      case vk_cmd_op::OP_DISPATCH_INDIRECT: {
        vk_dispatch_indirect_cmd c{};
        if (read_buf_mem(d, op.handle, op.vb_offs[0], &c, sizeof(c))) {
          emit_dispatch(c.x, c.y, c.z);
        }
      } break;
      case vk_cmd_op::OP_COPY_BUF:
        sync_buf_mem(d, op.handle);
        if (g_vk_hooks_set && g_vk_hooks.copy_buffer) {
          g_vk_hooks.copy_buffer(op.handle, op.dst_handle, op.vb_offs[0], op.vb_offs[1],
                                 op.vb_offs[2], g_vk_hooks.user);
        }
        break;
      case vk_cmd_op::OP_COPY_BUF_IMG: {
        sync_buf_mem(d, op.handle);
        const void* cptr = nullptr;
        u64 clen = 0;
        if (d->bufs.count(op.handle)) {
          const auto& b = d->bufs[op.handle];
          if (b.mem_id && d->mems.count(b.mem_id)) {
            const auto& m = d->mems[b.mem_id].data;
            u64 off = b.mem_off + op.bic.bufferOffset;
            if (off < m.size()) {
              cptr = m.data() + off;
              clen = (u64)(m.size() - off);
            }
          }
        }
        if (g_vk_hooks_set && g_vk_hooks.copy_buf_to_img) {
          g_vk_hooks.copy_buf_to_img(
              op.handle, op.dst_handle, op.bic.bufferOffset,
              op.bic.imageExtent.width, op.bic.imageExtent.height,
              op.bic.imageSubresource.mipLevel, op.bic.imageSubresource.baseArrayLayer,
              cptr, clen, g_vk_hooks.user);
        }
      } break;
      case vk_cmd_op::OP_UPDATE_BUF:
        if (!op.inline_bytes.empty()) {
          write_buf_mem(d, op.handle, op.vb_offs[0], op.inline_bytes.data(), op.inline_bytes.size());
          sync_buf_mem(d, op.handle);
        }
        break;
      case vk_cmd_op::OP_FILL_BUF: {
        auto bit = d->bufs.find(op.handle);
        if (bit != d->bufs.end() && bit->second.mem_id && d->mems.count(bit->second.mem_id)) {
          const auto& b = bit->second;
          auto& m = d->mems[b.mem_id].data;
          u64 off = op.vb_offs[0];
          u64 len = (op.vb_offs[1] == ~0ull && off < b.size) ? (b.size - off) : op.vb_offs[1];
          u64 pos = b.mem_off + off;
          if (off < b.size && pos < m.size()) {
            u64 n = std::min<u64>({len, b.size - off, m.size() - pos});
            for (u64 k = 0; k + 4 <= n; k += 4) memcpy(m.data() + pos + k, &op.a0, 4);
            sync_buf_mem(d, op.handle);
          }
        }
      } break;
      case vk_cmd_op::OP_SET_EVENT: d->events[op.handle] = true; break;
      case vk_cmd_op::OP_RESET_EVENT: d->events[op.handle] = false; break;
      case vk_cmd_op::OP_EXEC_CMDS:
        for (void* scb : op.sec_cbs) {
          exec_cmdbuf_state(d, (const vk_cmdbuf_obj*)scb, st, depth + 1);
        }
        break;
    }
  }
}

static void exec_cmdbuf(vk_device_obj* d, vk_cmdbuf_obj* cb) {
  vk_exec_state st;
  exec_cmdbuf_state(d, cb, st, 0);
}

struct vk_proc_entry { const char* name; void* fn; };
extern const vk_proc_entry k_vk_procs[];
extern const size_t k_vk_proc_count;

// ---- Vulkan API Implementations (64-bit Win64 ABI) -------------------------
i32 TRT_GUEST_ABI trt_vkEnumerateInstanceVersion(u32* pVer) {
  if (pVer) *pVer = VK_MAKE_VER(1, 3, 255);
  return VK_SUCCESS_V;
}

i32 TRT_GUEST_ABI trt_vkEnumerateInstanceExtensionProperties(const char*, u32* pCount,
                                                             vk_ext_props* pProps) {
  static const vk_ext_props k_exts[] = {
      {"VK_KHR_surface", 25},
      {"VK_KHR_win32_surface", 6},
      {"VK_KHR_get_physical_device_properties2", 2},
      {"VK_KHR_external_memory_capabilities", 1},
      {"VK_KHR_external_semaphore_capabilities", 1},
      {"VK_KHR_external_fence_capabilities", 1},
      {"VK_KHR_device_group_creation", 1},
      {"VK_EXT_debug_utils", 2},
  };
  const u32 n = (u32)(sizeof(k_exts) / sizeof(k_exts[0]));
  if (!pProps) { if (pCount) *pCount = n; return VK_SUCCESS_V; }
  u32 cap = pCount ? *pCount : 0;
  u32 cp = std::min(cap, n);
  for (u32 i = 0; i < cp; ++i) pProps[i] = k_exts[i];
  if (pCount) *pCount = cp;
  return (cp < n) ? VK_INCOMPLETE_V : VK_SUCCESS_V;
}

i32 TRT_GUEST_ABI trt_vkEnumerateInstanceLayerProperties(u32* pCount, vk_layer_props*) {
  if (pCount) *pCount = 0;
  return VK_SUCCESS_V;
}

i32 TRT_GUEST_ABI trt_vkCreateInstance(const vk_instance_create_info* ci, const void*,
                                       void** pInstance) {
  if (!ci || !pInstance) return VK_ERROR_INITIALIZATION_FAILED_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_instance_obj* inst = new vk_instance_obj();
  inst->gpu.instance = inst;
  g_instances.push_back(inst);
  *pInstance = inst;
  vk_log("vulkan-1: vkCreateInstance -> Vulkan 1.3 ICD backed by SPIR-V->HLSL/D3D11");
  return VK_SUCCESS_V;
}

void TRT_GUEST_ABI trt_vkDestroyInstance(void* inst, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  auto it = std::find(g_instances.begin(), g_instances.end(), (vk_instance_obj*)inst);
  if (it != g_instances.end()) {
    delete *it;
    g_instances.erase(it);
  }
}

i32 TRT_GUEST_ABI trt_vkEnumeratePhysicalDevices(void* inst_v, u32* pCount, void** pDevs) {
  vk_instance_obj* inst = (vk_instance_obj*)inst_v;
  if (!inst) return VK_ERROR_INITIALIZATION_FAILED_V;
  if (!pDevs) { if (pCount) *pCount = 1; return VK_SUCCESS_V; }
  if (pCount && *pCount >= 1) { pDevs[0] = &inst->gpu; *pCount = 1; return VK_SUCCESS_V; }
  return VK_INCOMPLETE_V;
}

// Desktop-class limits (AMD Navi 21 class, consistent with the vendorID/
// deviceID above). Values are the SPIR-V->HLSL->D3D11 bridge's honest
// capability floor: everything here is either truly supported by the D3D11
// path or enforced by the shader translator. Zeroed limits previously made
// Godot compute "maximum varyings = 0" and fail EVERY shader compile.
static void fill_phys_dev_limits(VkPhysicalDeviceLimits* L) {
  *L = {};
  L->maxImageDimension1D = 16384;
  L->maxImageDimension2D = 16384;
  L->maxImageDimension3D = 2048;
  L->maxImageDimensionCube = 16384;
  L->maxImageArrayLayers = 2048;
  L->maxTexelBufferElements = 0x8000000ull < (1ull << 32) ? 134217728u : 134217728u;
  L->maxUniformBufferRange = 65536;
  L->maxStorageBufferRange = 134217728;
  L->maxPushConstantsSize = 256;
  L->maxMemoryAllocationCount = 4294967295u;  // D3D11: no hard allocation cap
  L->maxSamplerAllocationCount = 2048;
  L->bufferImageGranularity = 4096;
  L->sparseAddressSpaceSize = 0;  // sparse not bridged (fail-visible elsewhere)
  L->maxBoundDescriptorSets = 8;
  L->maxPerStageDescriptorSamplers = 16;
  L->maxPerStageDescriptorUniformBuffers = 15;   // D3D11.1 b-constraint
  L->maxPerStageDescriptorStorageBuffers = 8;
  L->maxPerStageDescriptorSampledImages = 128;
  L->maxPerStageDescriptorStorageImages = 8;
  L->maxPerStageDescriptorInputAttachments = 8;
  L->maxPerStageResources = 128;
  L->maxDescriptorSetSamplers = 48;
  L->maxDescriptorSetUniformBuffers = 90;
  L->maxDescriptorSetUniformBuffersDynamic = 15;
  L->maxDescriptorSetStorageBuffers = 72;
  L->maxDescriptorSetStorageBuffersDynamic = 24;
  L->maxDescriptorSetSampledImages = 768;
  L->maxDescriptorSetStorageImages = 72;
  L->maxDescriptorSetInputAttachments = 72;
  L->maxVertexInputAttributes = 32;
  L->maxVertexInputBindings = 32;
  L->maxVertexInputAttributeOffset = 2047;   // D3D11: 4GB-1 really, keep sane
  L->maxVertexInputBindingStride = 2048;     // D3D11: 4096 for 11.1; conservative
  L->maxVertexOutputComponents = 128;        // D3D11: 32x4-vs-out
  L->maxTessellationGenerationLevel = 64;
  L->maxTessellationPatchSize = 32;
  L->maxTessellationControlPerVertexInputComponents = 128;
  L->maxTessellationControlPerVertexOutputComponents = 128;
  L->maxTessellationControlPerPatchOutputComponents = 120;
  L->maxTessellationControlTotalOutputComponents = 4096;
  L->maxTessellationEvaluationInputComponents = 128;
  L->maxTessellationEvaluationOutputComponents = 128;
  L->maxGeometryShaderInvocations = 32;
  L->maxGeometryInputComponents = 64;
  L->maxGeometryOutputComponents = 128;
  L->maxGeometryOutputVertices = 1024;
  L->maxGeometryTotalOutputComponents = 1024;
  // The load-bearing one: Godot computes its varying budget from this.
  L->maxFragmentInputComponents = 128;       // D3D11: 32x4-ps-in
  L->maxFragmentOutputAttachments = 8;
  L->maxFragmentDualSrcAttachments = 1;
  L->maxFragmentCombinedOutputResources = 8;
  L->maxComputeSharedMemorySize = 32768;
  L->maxComputeWorkGroupCount[0] = 65535;
  L->maxComputeWorkGroupCount[1] = 65535;
  L->maxComputeWorkGroupCount[2] = 65535;
  L->maxComputeWorkGroupInvocations = 1024;
  L->maxComputeWorkGroupSize[0] = 1024;
  L->maxComputeWorkGroupSize[1] = 1024;
  L->maxComputeWorkGroupSize[2] = 64;
  L->subPixelPrecisionBits = 8;
  L->subTexelPrecisionBits = 8;
  L->mipmapPrecisionBits = 8;
  L->maxDrawIndexedIndexValue = 4294967294u;  // D3D11: 32-bit indices
  L->maxDrawIndirectCount = 65535;
  L->maxSamplerLodBias = 16.0f;
  L->maxSamplerAnisotropy = 16.0f;
  L->maxViewports = 16;
  L->maxViewportDimensions[0] = 16384;
  L->maxViewportDimensions[1] = 16384;
  L->viewportBoundsRange[0] = -32768.0f;
  L->viewportBoundsRange[1] = 32767.0f;
  L->viewportSubPixelBits = 8;
  L->minMemoryMapAlignment = 4096;
  L->minTexelBufferOffsetAlignment = 256;
  L->minUniformBufferOffsetAlignment = 16;   // D3D11 constant-buffer 16B
  L->minStorageBufferOffsetAlignment = 16;
  L->minTexelOffset = -32;
  L->maxTexelOffset = 31;
  L->minTexelGatherOffset = -32;
  L->maxTexelGatherOffset = 31;
  L->minInterpolationOffset = -0.5f;
  L->maxInterpolationOffset = 0.5f;
  L->subPixelInterpolationOffsetBits = 8;
  L->maxFramebufferWidth = 16384;
  L->maxFramebufferHeight = 16384;
  L->maxFramebufferLayers = 2048;
  L->framebufferColorSampleCounts = 0x7F;    // 1|2|4|8|16|32|64
  L->framebufferDepthSampleCounts = 0x7F;
  L->framebufferStencilSampleCounts = 0x7F;
  L->framebufferNoAttachmentsSampleCounts = 0x7F;
  L->maxColorAttachments = 8;
  L->sampledImageColorSampleCounts = 0x7F;
  L->sampledImageIntegerSampleCounts = 0x7F;
  L->sampledImageDepthSampleCounts = 0x7F;
  L->sampledImageStencilSampleCounts = 0x7F;
  L->storageImageSampleCounts = 0x3F;
  L->maxSampleMaskWords = 1;
  L->timestampComputeAndGraphics = VK_TRUE;
  L->timestampPeriod = 1.0f;                 // 1 ns (D3D11 Query timestamp)
  L->maxClipDistances = 8;
  L->maxCullDistances = 8;
  L->maxCombinedClipAndCullDistances = 8;
  L->discreteQueuePriorities = 2;
  L->pointSizeRange[0] = 0.0f;
  L->pointSizeRange[1] = 8191.0f;
  L->lineWidthRange[0] = 1.0f;
  L->lineWidthRange[1] = 8.0f;
  L->pointSizeGranularity = 0.0625f;
  L->lineWidthGranularity = 0.0625f;
  L->strictLines = VK_FALSE;
  L->standardSampleLocations = VK_TRUE;
  L->optimalBufferCopyOffsetAlignment = 1;
  L->optimalBufferCopyRowPitchAlignment = 1;
  L->nonCoherentAtomSize = 256;
}

void TRT_GUEST_ABI trt_vkGetPhysicalDeviceProperties(void*, VkPhysicalDeviceProperties* p) {
  if (!p) return;
  memset(p, 0, sizeof(*p));
  p->apiVersion = VK_MAKE_VER(1, 3, 255);
  p->driverVersion = 1;
  p->vendorID = 0x1002u;
  p->deviceID = 0x73DFu;
  p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
  snprintf(p->deviceName, sizeof(p->deviceName), "Tiramisu Vulkan->D3D11");
  fill_phys_dev_limits(&p->limits);
  p->sparseProperties.residencyStandard2DBlockShape = VK_FALSE;
  // sparse not bridged: all sparse properties honestly false (zeroed)
}
void TRT_GUEST_ABI trt_vkGetPhysicalDeviceProperties2(void* pd, VkPhysicalDeviceProperties2* p2) {
  if (p2) trt_vkGetPhysicalDeviceProperties(pd, &p2->properties);
}
// ---- Honest feature reporting (shared by queries + vkCreateDevice) --------
// Every feature struct here is (sType, pNext, N x VkBool32) with the first
// bool at offset 16. The fill functions zero the bool region and set only
// the bits the ICD/backend genuinely implements; the validation helpers
// compare a request against the same honest fill, so a game can never be
// told a feature exists and then be rejected for requesting it (or the
// reverse). One source of truth for both directions.
//
// Evidence for the FALSE bits (verified 2026-11-20 against this tree):
//  - geometry/tessellation stages: the pipeline builder maps every stage to
//    VS or PS (no GS/HS/DS path; trt_vk_pipeline_desc has vs/ps/cs only).
//  - fragment/vertex stores+atomics, 64-bit atomics: the SPIR-V translator
//    has no OpAtomic* handling at all.
//  - shaderImageGatherExtended: no OpImageGather in the translator.
//  - multiViewport: vkCmdSetViewport records vp[0] only; one vp per draw.
//  - occlusionQueryPrecise/pipelineStatisticsQuery/hostQueryReset: query
//    pools return zeros (vkGetQueryPoolResults memsets 0; Begin/EndQuery
//    are no-ops).
//  - descriptor indexing (all): not implemented (later batch).
//  - bufferDeviceAddress: the translator has no PhysicalStorageBuffer
//    support, so the addresses vkGetBufferDeviceAddress returns cannot be
//    dereferenced by any shader.
//  - samplerYcbcrConversion: creation returns an id but no conversion is
//    applied anywhere in sampling.
//  - shaderDrawParameters: no DrawId/BaseVertex/BaseInstance in the
//    translator. 16-bit/8-bit storage, subgroups, inline uniform block,
//    private data, demote/terminate, dot product, ASTC-HDR, scalar block
//    layout, separate depth/stencil layouts, imageless framebuffer (no
//    attachment-begin-info path), vulkanMemoryModel: none implemented.
// TRUE bits and why:
//  - dynamicRendering: vkCmdBeginRendering records a real OP_BEGIN_RENDERING
//    with attachments + clears, executed by the backend.
//  - synchronization2: vkQueueSubmit2 executes synchronously; barrier2 is
//    a no-op, which is correct on this single-queue in-order device.
//  - timelineSemaphore: real per-semaphore counters (create/signal/get);
//    waits are trivially satisfied because submits execute synchronously.
//  - drawIndirectCount: exec reads the count buffer, clamps, and draws.
//  - samplerMirrorClampToEdge: vkCreateSampler stores the address modes and
//    the D3D11 backend maps mode 4 to D3D11_TEXTURE_ADDRESS_MIRROR_ONCE.

// VkStructureType values (Khronos; the KHR aliases share the same value).
static constexpr u32 VK_STYPE_VULKAN_11_FEATURES_V = 49u;
static constexpr u32 VK_STYPE_VULKAN_12_FEATURES_V = 51u;
static constexpr u32 VK_STYPE_VULKAN_13_FEATURES_V = 53u;
static constexpr u32 VK_STYPE_DESCRIPTOR_INDEXING_FEATURES_V = 1000161001u;
static constexpr u32 VK_STYPE_TIMELINE_SEMAPHORE_FEATURES_V = 1000207000u;
static constexpr u32 VK_STYPE_BUFFER_DEVICE_ADDRESS_FEATURES_V = 1000257000u;
static constexpr u32 VK_STYPE_SYNCHRONIZATION_2_FEATURES_V = 1000314007u;
static constexpr u32 VK_STYPE_DYNAMIC_RENDERING_FEATURES_V = 1000044003u;

// Header shared by every pNext-chained struct: sType at 0, pNext at 8.
struct vk_pnext_hdr { u32 sType; const void* pNext; };

static void fill_vk_features(VkPhysicalDeviceFeatures* f) {
  if (!f) return;
  *f = {};
  // Features actually honored by the SPIR-V->HLSL->D3D11 bridge; the rest
  // stay false rather than promising what the bridge can't deliver.
  // Godot's _check_device_features hard-gates on imageCubeArray +
  // independentBlend (rendering_device_driver_vulkan.cpp:_check_device_features
  // — measured live: alert "does not support the following features" ->
  // Vulkan init aborted -> forced GL fallback). Both are real D3D11
  // capabilities the bridge carries.
  f->robustBufferAccess = VK_TRUE;
  f->fullDrawIndexUint32 = VK_TRUE;
  f->independentBlend = VK_TRUE;
  f->imageCubeArray = VK_TRUE;
  f->dualSrcBlend = VK_TRUE;
  f->sampleRateShading = VK_TRUE;
  f->shaderClipDistance = VK_TRUE;
  f->shaderCullDistance = VK_TRUE;
  f->shaderStorageImageExtendedFormats = VK_TRUE;
  f->depthClamp = VK_TRUE;
  f->depthBiasClamp = VK_TRUE;
  f->fillModeNonSolid = VK_TRUE;
  f->samplerAnisotropy = VK_TRUE;
  f->textureCompressionBC = VK_TRUE;
  // Deliberately FALSE (were TRUE; the stages/ops don't exist — see the
  // evidence block above): geometryShader, tessellationShader,
  // shaderTessellationAndGeometryPointSize, fragmentStoresAndAtomics,
  // vertexPipelineStoresAndAtomics, shaderImageGatherExtended,
  // multiViewport, occlusionQueryPrecise, pipelineStatisticsQuery.
  f->shaderFloat64 = VK_FALSE;  // D3D11: 64-bit shader floats are optional
  f->shaderInt64 = VK_FALSE;
  f->sparseBinding = VK_FALSE;  // sparse not bridged
}

// Each fill zeroes the struct's bool region, then sets the honestly
// supported bits by name. (memset spans sizeof()-16, which includes the
// trailing alignment padding — harmless, it is inside the struct.)
static void fill_vulkan11_features(VkPhysicalDeviceVulkan11Features* f) {
  memset(&f->storageBuffer16BitAccess, 0,
         sizeof(*f) - offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess));
  // All FALSE: no 16-bit storage, multiview, variable pointers, protected
  // memory, YCbCr conversion, or DrawParameters.
}
static void fill_vulkan12_features(VkPhysicalDeviceVulkan12Features* f) {
  memset(&f->samplerMirrorClampToEdge, 0,
         sizeof(*f) - offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge));
  f->samplerMirrorClampToEdge = VK_TRUE;
  f->drawIndirectCount = VK_TRUE;
  f->timelineSemaphore = VK_TRUE;
  // Everything else FALSE: no descriptor indexing, buffer device address,
  // 8/16-bit storage, subgroups, or Vulkan memory model.
}
static void fill_vulkan13_features(VkPhysicalDeviceVulkan13Features* f) {
  memset(&f->robustImageAccess, 0,
         sizeof(*f) - offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess));
  f->synchronization2 = VK_TRUE;
  f->dynamicRendering = VK_TRUE;
  // Everything else FALSE: no inline uniform block, private data,
  // demote/terminate, subgroups, ASTC-HDR, dot product, maintenance4.
}
static void fill_descriptor_indexing_features(VkPhysicalDeviceDescriptorIndexingFeatures* f) {
  memset(&f->shaderInputAttachmentArrayDynamicIndexing, 0,
         sizeof(*f) - offsetof(VkPhysicalDeviceDescriptorIndexingFeatures,
                               shaderInputAttachmentArrayDynamicIndexing));
  // All FALSE: descriptor indexing is a later batch.
}
static void fill_buffer_device_address_features(VkPhysicalDeviceBufferDeviceAddressFeatures* f) {
  memset(&f->bufferDeviceAddress, 0,
         sizeof(*f) - offsetof(VkPhysicalDeviceBufferDeviceAddressFeatures, bufferDeviceAddress));
  // All FALSE: no PhysicalStorageBuffer support in the translator.
}
static void fill_dynamic_rendering_features(VkPhysicalDeviceDynamicRenderingFeatures* f) {
  f->dynamicRendering = VK_TRUE;
}
static void fill_synchronization2_features(VkPhysicalDeviceSynchronization2Features* f) {
  f->synchronization2 = VK_TRUE;
}
static void fill_timeline_semaphore_features(VkPhysicalDeviceTimelineSemaphoreFeatures* f) {
  f->timelineSemaphore = VK_TRUE;
}

// Fills a known feature struct in place (sType/pNext untouched). Returns
// false for unknown sTypes — the caller leaves those alone.
static bool fill_feature_struct(u32 sType, void* s) {
  switch (sType) {
    case VK_STYPE_VULKAN_11_FEATURES_V:
      fill_vulkan11_features((VkPhysicalDeviceVulkan11Features*)s); return true;
    case VK_STYPE_VULKAN_12_FEATURES_V:
      fill_vulkan12_features((VkPhysicalDeviceVulkan12Features*)s); return true;
    case VK_STYPE_VULKAN_13_FEATURES_V:
      fill_vulkan13_features((VkPhysicalDeviceVulkan13Features*)s); return true;
    case VK_STYPE_DESCRIPTOR_INDEXING_FEATURES_V:
      fill_descriptor_indexing_features((VkPhysicalDeviceDescriptorIndexingFeatures*)s);
      return true;
    case VK_STYPE_BUFFER_DEVICE_ADDRESS_FEATURES_V:
      fill_buffer_device_address_features((VkPhysicalDeviceBufferDeviceAddressFeatures*)s);
      return true;
    case VK_STYPE_DYNAMIC_RENDERING_FEATURES_V:
      fill_dynamic_rendering_features((VkPhysicalDeviceDynamicRenderingFeatures*)s);
      return true;
    case VK_STYPE_SYNCHRONIZATION_2_FEATURES_V:
      fill_synchronization2_features((VkPhysicalDeviceSynchronization2Features*)s);
      return true;
    case VK_STYPE_TIMELINE_SEMAPHORE_FEATURES_V:
      fill_timeline_semaphore_features((VkPhysicalDeviceTimelineSemaphoreFeatures*)s);
      return true;
    default: return false;
  }
}

// Exact VkBool32 field count for a known feature struct, 0 if unknown.
// NOTE: not (sizeof-16)/4 — several structs carry trailing alignment
// padding (e.g. Vulkan12 is 208 B but has 47 bools), and comparing the
// padding slot would reject callers for garbage they never set.
static u32 feature_struct_bool_count(u32 sType) {
  switch (sType) {
    case VK_STYPE_VULKAN_11_FEATURES_V: return 12;
    case VK_STYPE_VULKAN_12_FEATURES_V: return 47;
    case VK_STYPE_VULKAN_13_FEATURES_V: return 15;
    case VK_STYPE_DESCRIPTOR_INDEXING_FEATURES_V: return 20;
    case VK_STYPE_BUFFER_DEVICE_ADDRESS_FEATURES_V: return 3;
    case VK_STYPE_DYNAMIC_RENDERING_FEATURES_V: return 1;
    case VK_STYPE_SYNCHRONIZATION_2_FEATURES_V: return 1;
    case VK_STYPE_TIMELINE_SEMAPHORE_FEATURES_V: return 1;
    default: return 0;
  }
}

// True when every feature the requester asks for (non-zero VkBool32) is one
// the ICD honestly implements. Unknown structs are not rejected: a newer
// extension's features are outside what this ICD can judge, and the
// extension-name check in vkCreateDevice is the gate for those.
static bool feature_struct_request_supported(u32 sType, const void* req) {
  const u32 n = feature_struct_bool_count(sType);
  if (n == 0) return true;
  // Honest reference: fill a scratch copy, then compare bool-by-bool.
  // The biggest known struct is Vulkan12 (208 B); 256 B covers all.
  alignas(8) u8 honest[256];
  memset(honest, 0, sizeof(honest));
  if (!fill_feature_struct(sType, honest)) return true;
  const u32* r = (const u32*)((const u8*)req + 16);
  const u32* h = (const u32*)(honest + 16);
  for (u32 i = 0; i < n; ++i) {
    if (r[i] != 0 && h[i] == 0) return false;
  }
  return true;
}

static bool base_features_request_supported(const VkPhysicalDeviceFeatures* req) {
  VkPhysicalDeviceFeatures honest{};
  fill_vk_features(&honest);
  const u32* r = (const u32*)req;
  const u32* h = (const u32*)&honest;
  constexpr u32 n = (u32)(sizeof(VkPhysicalDeviceFeatures) / sizeof(u32));  // 55
  for (u32 i = 0; i < n; ++i) {
    if (r[i] != 0 && h[i] == 0) return false;
  }
  return true;
}

void TRT_GUEST_ABI trt_vkGetPhysicalDeviceFeatures(void*, VkPhysicalDeviceFeatures* f) {
  fill_vk_features(f);
}
void TRT_GUEST_ABI trt_vkGetPhysicalDeviceFeatures2(void* pd, VkPhysicalDeviceFeatures2* f2) {
  if (!f2) return;
  trt_vkGetPhysicalDeviceFeatures(pd, &f2->features);
  // Walk the pNext chain and fill every known feature struct honestly.
  // Unknown structs are left untouched (forward-compat).
  for (const vk_pnext_hdr* p = (const vk_pnext_hdr*)f2->pNext; p;
       p = (const vk_pnext_hdr*)p->pNext) {
    fill_feature_struct(p->sType, (void*)p);
  }
}
void TRT_GUEST_ABI trt_vkGetPhysicalDeviceFormatProperties(void*, u32, vk_fmt_props* p) {
  if (p) {
    p->linearTilingFeatures = 0x1FFFFu;
    p->optimalTilingFeatures = 0x1FFFFu;
    p->bufferFeatures = 0x1FFFFu;
  }
}
void TRT_GUEST_ABI trt_vkGetPhysicalDeviceFormatProperties2(void* pd, u32 fmt, vk_fmt_props2* p2) {
  if (p2) trt_vkGetPhysicalDeviceFormatProperties(pd, fmt, &p2->formatProperties);
}
i32 TRT_GUEST_ABI trt_vkGetPhysicalDeviceImageFormatProperties(
    void*, u32, u32 type, u32, u32, u32, vk_img_fmt_props* p) {
  if (p) {
    u32 max_dim = (type == 2) ? 2048u : 16384u;
    p->maxExtent = {max_dim, (type == 0) ? 1u : max_dim, (type == 2) ? 2048u : 1u};
    p->maxMipLevels = (type == 2) ? 12u : 15u;
    p->maxArrayLayers = (type == 2) ? 1u : 2048u;
    p->sampleCounts = 0xFu;
    p->maxResourceSize = 1024ull * 1024ull * 1024ull;
  }
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkGetPhysicalDeviceImageFormatProperties2(
    void* pd, const vk_phys_dev_img_fmt_info2* info, vk_img_fmt_props2* p2) {
  if (!info || !p2) return VK_ERROR_INITIALIZATION_FAILED_V;
  return trt_vkGetPhysicalDeviceImageFormatProperties(
      pd, info->format, info->type, info->tiling, info->usage, info->flags,
      &p2->imageFormatProperties);
}
void TRT_GUEST_ABI trt_vkGetPhysicalDeviceSparseImageFormatProperties(
    void*, u32, u32, u32, u32, u32, u32* pCount, vk_sparse_img_fmt_props*) {
  if (pCount) *pCount = 0;
}
void TRT_GUEST_ABI trt_vkGetPhysicalDeviceSparseImageFormatProperties2(
    void*, const void*, u32* pCount, vk_sparse_img_fmt_props2*) {
  if (pCount) *pCount = 0;
}
void TRT_GUEST_ABI trt_vkGetPhysicalDeviceExternalBufferProperties(
    void*, const void*, vk_ext_handle_props* p) {
  if (p) {
    p->exportFromImportedHandleTypes = 0;
    p->compatibleHandleTypes = 0;
    p->features = 0;
  }
}
void TRT_GUEST_ABI trt_vkGetPhysicalDeviceExternalSemaphoreProperties(
    void*, const void*, vk_ext_handle_props* p) {
  if (p) {
    p->exportFromImportedHandleTypes = 0;
    p->compatibleHandleTypes = 0;
    p->features = 0;
  }
}
void TRT_GUEST_ABI trt_vkGetPhysicalDeviceExternalFenceProperties(
    void*, const void*, vk_ext_handle_props* p) {
  if (p) {
    p->exportFromImportedHandleTypes = 0;
    p->compatibleHandleTypes = 0;
    p->features = 0;
  }
}
i32 TRT_GUEST_ABI trt_vkEnumeratePhysicalDeviceGroups(
    void* inst_v, u32* pCount, vk_phys_dev_group_props* pGroups) {
  vk_instance_obj* inst = (vk_instance_obj*)inst_v;
  if (!inst) return VK_ERROR_INITIALIZATION_FAILED_V;
  if (!pGroups) { if (pCount) *pCount = 1; return VK_SUCCESS_V; }
  if (pCount && *pCount >= 1) {
    memset(&pGroups[0].physicalDevices, 0, sizeof(pGroups[0].physicalDevices));
    pGroups[0].physicalDeviceCount = 1;
    pGroups[0].physicalDevices[0] = &inst->gpu;
    pGroups[0].subsetAllocation = 0;
    *pCount = 1;
    return VK_SUCCESS_V;
  }
  return VK_INCOMPLETE_V;
}
void TRT_GUEST_ABI trt_vkGetPhysicalDeviceQueueFamilyProperties(void*, u32* pCount, vk_qf_props* p) {
  if (!p) { if (pCount) *pCount = 1; return; }
  if (pCount && *pCount >= 1) {
    *pCount = 1;
    p[0] = {0xFu /*GFX|COMPUTE|TRANSFER|SPARSE*/, 4, 64, {1, 1, 1}};
  }
}
void TRT_GUEST_ABI trt_vkGetPhysicalDeviceQueueFamilyProperties2(void*, u32* pCount, vk_qf_props2* p2) {
  if (!p2) { if (pCount) *pCount = 1; return; }
  if (pCount && *pCount >= 1) {
    *pCount = 1;
    p2[0].queueFamilyProperties = {0xFu, 4, 64, {1, 1, 1}};
  }
}
void TRT_GUEST_ABI trt_vkGetPhysicalDeviceMemoryProperties(void*, vk_mem_props* p) {
  if (!p) return;
  memset(p, 0, sizeof(*p));
  p->memoryTypeCount = 2;
  p->memoryTypes[0] = {0x1u /*DEVICE_LOCAL*/, 0};
  p->memoryTypes[1] = {0xFu /*HOST_VISIBLE|COHERENT|CACHED|LOCAL*/, 0};
  p->memoryHeapCount = 1;
  p->memoryHeaps[0] = {2ull * 1024ull * 1024ull * 1024ull, 1u};
}
void TRT_GUEST_ABI trt_vkGetPhysicalDeviceMemoryProperties2(void* pd, vk_mem_props2* p2) {
  if (p2) trt_vkGetPhysicalDeviceMemoryProperties(pd, &p2->memoryProperties);
}
// Device extensions the ICD advertises. File scope so vkCreateDevice
// validation checks requests against exactly this list (one source of truth).
static const vk_ext_props k_vk_dev_exts[] = {
    {"VK_KHR_swapchain", 70},
    {"VK_KHR_dynamic_rendering", 1},
    {"VK_KHR_synchronization2", 1},
    {"VK_KHR_maintenance1", 2},
    {"VK_KHR_maintenance2", 1},
    {"VK_KHR_maintenance3", 1},
    {"VK_KHR_create_renderpass2", 1},
    {"VK_KHR_timeline_semaphore", 2},
    {"VK_KHR_get_memory_requirements2", 1},
    {"VK_KHR_bind_memory2", 1},
    {"VK_KHR_sampler_ycbcr_conversion", 14},
    {"VK_KHR_descriptor_update_template", 1},
    {"VK_KHR_draw_indirect_count", 1},
    {"VK_KHR_buffer_device_address", 1},
    {"VK_KHR_push_descriptor", 2},
};
static constexpr u32 k_vk_dev_ext_count =
    (u32)(sizeof(k_vk_dev_exts) / sizeof(k_vk_dev_exts[0]));

static bool vk_dev_ext_supported(const char* name) {
  if (!name) return false;
  for (u32 i = 0; i < k_vk_dev_ext_count; ++i) {
    if (strcmp(k_vk_dev_exts[i].extensionName, name) == 0) return true;
  }
  return false;
}

i32 TRT_GUEST_ABI trt_vkEnumerateDeviceExtensionProperties(void*, const char*, u32* pCount,
                                                           vk_ext_props* pProps) {
  const u32 n = k_vk_dev_ext_count;
  if (!pProps) { if (pCount) *pCount = n; return VK_SUCCESS_V; }
  u32 cap = pCount ? *pCount : 0;
  u32 cp = std::min(cap, n);
  for (u32 i = 0; i < cp; ++i) pProps[i] = k_vk_dev_exts[i];
  if (pCount) *pCount = cp;
  return (cp < n) ? VK_INCOMPLETE_V : VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkEnumerateDeviceLayerProperties(void*, u32* c, vk_layer_props*) {
  if (c) *c = 0; return VK_SUCCESS_V;
}

// ---- WSI -------------------------------------------------------------------
i32 TRT_GUEST_ABI trt_vkCreateWin32SurfaceKHR(void*, const vk_win32_surface_ci* ci,
                                              const void*, u64* pSurface) {
  if (!pSurface) return VK_ERROR_INITIALIZATION_FAILED_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  u64 id = g_next_surface++;
  g_surfaces[id].hwnd = ci ? ci->hwnd : nullptr;
  *pSurface = id;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroySurfaceKHR(void*, u64 surf, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  g_surfaces.erase(surf);
}
i32 TRT_GUEST_ABI trt_vkGetPhysicalDeviceWin32PresentationSupportKHR(void*, u32) { return 1; }
i32 TRT_GUEST_ABI trt_vkGetPhysicalDeviceSurfaceSupportKHR(void*, u32, u64, u32* pSup) {
  if (pSup) *pSup = 1; return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(void*, u64, vk_surface_caps* p) {
  if (!p) return VK_ERROR_INITIALIZATION_FAILED_V;
  memset(p, 0, sizeof(*p));
  p->minImageCount = 2; p->maxImageCount = 4;
  int dw = 0, dh = 0;
  trt_display_size(&dw, &dh);
  p->currentExtent = {(u32)(dw > 0 ? dw : 1280), (u32)(dh > 0 ? dh : 720)};
  p->minImageExtent = {64, 64}; p->maxImageExtent = {4096, 4096};
  p->maxImageArrayLayers = 1;
  p->supportedTransforms = 1; p->currentTransform = 1;
  p->supportedCompositeAlpha = 1;
  p->supportedUsageFlags = 0x1Fu;
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkGetPhysicalDeviceSurfaceFormatsKHR(void*, u64, u32* pCount,
                                                            vk_surface_fmt* pFmts) {
  static const vk_surface_fmt k_fmts[] = {{44, 0}, {37, 0}, {50, 0}};
  const u32 n = 3;
  if (!pFmts) { if (pCount) *pCount = n; return VK_SUCCESS_V; }
  u32 cp = std::min(pCount ? *pCount : 0u, n);
  for (u32 i = 0; i < cp; ++i) pFmts[i] = k_fmts[i];
  if (pCount) *pCount = cp;
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkGetPhysicalDeviceSurfacePresentModesKHR(void*, u64, u32* pCount, u32* pModes) {
  static const u32 k_modes[2] = {2 /*FIFO*/, 1 /*MAILBOX*/};
  if (!pModes) { if (pCount) *pCount = 2; return VK_SUCCESS_V; }
  u32 cp = std::min(pCount ? *pCount : 0u, 2u);
  for (u32 i = 0; i < cp; ++i) pModes[i] = k_modes[i];
  if (pCount) *pCount = cp;
  return VK_SUCCESS_V;
}

i32 TRT_GUEST_ABI trt_vkCreateDevice(void*, const vk_dev_ci* ci, const void*, void** pDev) {
  if (!ci || !pDev) return VK_ERROR_INITIALIZATION_FAILED_V;
  // Validate before allocating anything: requested extensions must be ones
  // the ICD advertises, and requested features must be ones it implements.
  // The feature checks use the same honest fill as vkGetPhysicalDevice-
  // Features(2), so a game that asks for exactly what it was told exists
  // can never be rejected here.
  for (u32 i = 0; i < ci->enabledExtensionCount; ++i) {
    const char* name =
        ci->ppEnabledExtensionNames ? ci->ppEnabledExtensionNames[i] : nullptr;
    if (!vk_dev_ext_supported(name)) {
      vk_logf("vulkan: vkCreateDevice rejected - extension not present: %s",
              name ? name : "(null)");
      return VK_ERROR_EXTENSION_NOT_PRESENT_V;
    }
  }
  if (ci->pEnabledFeatures && !base_features_request_supported(ci->pEnabledFeatures)) {
    vk_log("vulkan: vkCreateDevice rejected - requested VkPhysicalDeviceFeatures "
           "not implemented by this ICD");
    return VK_ERROR_FEATURE_NOT_PRESENT_V;
  }
  for (const vk_pnext_hdr* p = (const vk_pnext_hdr*)ci->pNext; p;
       p = (const vk_pnext_hdr*)p->pNext) {
    if (!feature_struct_request_supported(p->sType, p)) {
      vk_logf("vulkan: vkCreateDevice rejected - requested feature struct "
              "sType %u asks for features this ICD does not implement",
              p->sType);
      return VK_ERROR_FEATURE_NOT_PRESENT_V;
    }
  }
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = new vk_device_obj();
  d->main_queue.device = d;
  g_devices.push_back(d);
  g_active_dev = d;
  *pDev = d;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyDevice(void* dev_v, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = (vk_device_obj*)dev_v;
  if (!d) return;
  for (auto* cb : d->cmdbufs) delete cb;
  if (g_active_dev == d) g_active_dev = nullptr;
  g_devices.erase(std::remove(g_devices.begin(), g_devices.end(), d), g_devices.end());
  delete d;
}
void TRT_GUEST_ABI trt_vkGetDeviceQueue(void* dev_v, u32, u32, void** pQueue) {
  vk_device_obj* d = dev_from(dev_v);
  if (pQueue && d) *pQueue = &d->main_queue;
}
void TRT_GUEST_ABI trt_vkGetDeviceQueue2(void* dev_v, const void*, void** pQueue) {
  trt_vkGetDeviceQueue(dev_v, 0, 0, pQueue);
}
i32 TRT_GUEST_ABI trt_vkDeviceWaitIdle(void*) { return VK_SUCCESS_V; }
i32 TRT_GUEST_ABI trt_vkQueueWaitIdle(void*) { return VK_SUCCESS_V; }

// ---- Swapchain & Presentation ----------------------------------------------
i32 TRT_GUEST_ABI trt_vkCreateSwapchainKHR(void* dev_v, const vk_swapchain_ci* ci,
                                           const void*, u64* pSc) {
  if (!ci || !pSc) return VK_ERROR_INITIALIZATION_FAILED_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 scid = d->next_h++;
  vk_swapchain_obj sc{};
  sc.id = scid;
  sc.w = std::max(1u, ci->imageExtent.width);
  sc.h = std::max(1u, ci->imageExtent.height);
  sc.fmt = ci->imageFormat ? ci->imageFormat : 44u;
  u32 cnt = std::max(2u, std::min(4u, ci->minImageCount));
  for (u32 i = 0; i < cnt; ++i) {
    u64 img_id = d->next_h++;
    vk_img_obj im{};
    im.id = img_id;
    im.desc = {1, sc.fmt, sc.w, sc.h, 1, 1, 1, ci->imageUsage, 0, 1, {}};
    d->imgs[img_id] = im;
    sync_img_obj(d, im);
    sc.images.push_back(img_id);
  }
  d->swapchains[scid] = std::move(sc);
  *pSc = scid;
  vk_logf("vulkan: vkCreateSwapchainKHR -> id=%llu %ux%u fmt=%u imgs=%u",
          (unsigned long long)scid, ci->imageExtent.width, ci->imageExtent.height,
          ci->imageFormat, cnt);
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroySwapchainKHR(void* dev_v, u64 sc, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->swapchains.erase(sc);
}
i32 TRT_GUEST_ABI trt_vkGetSwapchainImagesKHR(void* dev_v, u64 sc, u32* pCount, u64* pImgs) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  if (!d || !d->swapchains.count(sc)) return VK_ERROR_INITIALIZATION_FAILED_V;
  const auto& imgs = d->swapchains[sc].images;
  if (!pImgs) { if (pCount) *pCount = (u32)imgs.size(); return VK_SUCCESS_V; }
  u32 cp = std::min(pCount ? *pCount : 0u, (u32)imgs.size());
  for (u32 i = 0; i < cp; ++i) pImgs[i] = imgs[i];
  if (pCount) *pCount = cp;
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkAcquireNextImageKHR(void* dev_v, u64 sc, u64, u64, u64, u32* pIdx) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  if (!d || !d->swapchains.count(sc) || !pIdx) return VK_ERROR_INITIALIZATION_FAILED_V;
  auto& s = d->swapchains[sc];
  *pIdx = s.next_img % (u32)s.images.size();
  s.next_img++;
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkQueuePresentKHR(void*, const vk_present_info* pi) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  static u64 s_presents = 0;
  ++s_presents;
  if (s_presents <= 10 || s_presents % 60 == 0) {
    vk_logf("vulkan: vkQueuePresentKHR frame=%llu sc=%llu idx=%u",
            (unsigned long long)s_presents,
            (unsigned long long)((pi && pi->swapchainCount && pi->pSwapchains) ? pi->pSwapchains[0] : 0),
            (pi && pi->swapchainCount && pi->pImageIndices) ? pi->pImageIndices[0] : 0u);
  }
  if (pi && pi->swapchainCount > 0 && pi->pSwapchains && g_vk_hooks_set && g_vk_hooks.present) {
    u32 idx = pi->pImageIndices ? pi->pImageIndices[0] : 0;
    g_vk_hooks.present(pi->pSwapchains[0], idx, g_vk_hooks.user);
    if (pi->pResults) for (u32 i = 0; i < pi->swapchainCount; ++i) pi->pResults[i] = VK_SUCCESS_V;
  }
  return VK_SUCCESS_V;
}

// ---- Memory, Buffers, Images, Views, Samplers ------------------------------
i32 TRT_GUEST_ABI trt_vkAllocateMemory(void* dev_v, const vk_mem_alloc_info* ai,
                                       const void*, u64* pMem) {
  if (!ai || !pMem) return VK_ERROR_OUT_OF_HOST_MEMORY_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 id = d->next_h++;
  auto& m = d->mems[id];
  m.id = id;
  m.size = ai->allocationSize;
  m.mem_type = ai->memoryTypeIndex;
  // If memory is host-visible (type 1) and <= 16MB, allocate vector.
  // If device-local (type 0), do NOT preallocate a giant CPU buffer!
  if (ai->memoryTypeIndex != 0 && ai->allocationSize <= (16ull * 1024ull * 1024ull)) {
    try {
      m.data.resize((size_t)std::max<u64>(ai->allocationSize, 16u));
    } catch (...) {
      m.data.clear();
    }
  }
  char buf[128];
  snprintf(buf, sizeof(buf), "[vulkan] vkAllocateMemory id=%llu size=%llu type=%u host_vec=%zu",
           (unsigned long long)id, (unsigned long long)ai->allocationSize, ai->memoryTypeIndex, m.data.size());
  trt_core_log(buf);
  *pMem = id;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkFreeMemory(void* dev_v, u64 mem, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) {
    char buf[64];
    snprintf(buf, sizeof(buf), "[vulkan] vkFreeMemory id=%llu", (unsigned long long)mem);
    trt_core_log(buf);
    d->mems.erase(mem);
  }
}
i32 TRT_GUEST_ABI trt_vkMapMemory(void* dev_v, u64 mem, u64 offset, u64 size, u32, void** ppData) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  if (!d || !d->mems.count(mem) || !ppData) return VK_ERROR_OUT_OF_DEVICE_MEMORY_V;
  auto& m = d->mems[mem];
  // Device-local memory (type 0) is not host-visible: mapping it is an
  // application error in real Vulkan. Refuse instead of allocating a
  // whole-block host vector for a map that must never happen.
  if (m.mem_type == 0) return VK_ERROR_MEMORY_MAP_FAILED_V;
  // Grow-to-need: map only the requested range (VK_WHOLE_SIZE = to end).
  u64 need = (size == ~0ULL) ? m.size : offset + size;
  if (need > m.size) need = m.size;
  if (m.data.size() < need && need > 0) {
    try {
      m.data.resize((size_t)std::max<u64>(need, 16u));
    } catch (...) {
      return VK_ERROR_OUT_OF_HOST_MEMORY_V;
    }
  }
  if (offset >= m.data.size()) return VK_ERROR_OUT_OF_DEVICE_MEMORY_V;
  m.mapped = true;
  *ppData = m.data.data() + offset;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkUnmapMemory(void* dev_v, u64 mem) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  if (d && d->mems.count(mem)) d->mems[mem].mapped = false;
}
i32 TRT_GUEST_ABI trt_vkFlushMappedMemoryRanges(void*, u32, const vk_mapped_range*) { return VK_SUCCESS_V; }
i32 TRT_GUEST_ABI trt_vkInvalidateMappedMemoryRanges(void*, u32, const vk_mapped_range*) { return VK_SUCCESS_V; }

i32 TRT_GUEST_ABI trt_vkCreateBuffer(void* dev_v, const vk_buf_ci* ci, const void*, u64* pBuf) {
  if (!ci || !pBuf) return VK_ERROR_OUT_OF_DEVICE_MEMORY_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 id = d->next_h++;
  d->bufs[id] = {id, ci->size, ci->usage, 0, 0};
  *pBuf = id;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyBuffer(void* dev_v, u64 buf, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->bufs.erase(buf);
  if (g_vk_hooks_set && g_vk_hooks.destroy_buffer) g_vk_hooks.destroy_buffer(buf, g_vk_hooks.user);
}
void TRT_GUEST_ABI trt_vkGetBufferMemoryRequirements(void* dev_v, u64 buf, vk_mem_reqs* mr) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 sz = (d && d->bufs.count(buf)) ? d->bufs[buf].size : 256u;
  if (mr) *mr = {((sz + 255ull) & ~255ull), 256, 0x3u};
}
void TRT_GUEST_ABI trt_vkGetBufferMemoryRequirements2(
    void* dev_v, const vk_buf_mem_reqs_info2* info, vk_mem_reqs2* mr2) {
  if (info && mr2) trt_vkGetBufferMemoryRequirements(dev_v, info->buffer, &mr2->memoryRequirements);
}
i32 TRT_GUEST_ABI trt_vkBindBufferMemory(void* dev_v, u64 buf, u64 mem, u64 off) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  if (d && d->bufs.count(buf)) {
    d->bufs[buf].mem_id = mem;
    d->bufs[buf].mem_off = off;
    sync_buf_mem(d, buf);
  }
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkBindBufferMemory2(void* dev_v, u32 cnt, const vk_bind_buf_mem_info* infos) {
  for (u32 i = 0; i < cnt && infos; ++i) {
    trt_vkBindBufferMemory(dev_v, infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
  }
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkCreateBufferView(void* dev_v, const vk_buf_view_ci* ci, const void*, u64* pView) {
  if (!ci || !pView) return VK_ERROR_OUT_OF_HOST_MEMORY_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  *pView = dev_from(dev_v)->next_h++;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyBufferView(void*, u64, const void*) {}
void TRT_GUEST_ABI trt_vkGetDeviceMemoryCommitment(void*, u64, u64* pBytes) {
  if (pBytes) *pBytes = 0;
}
void TRT_GUEST_ABI trt_vkGetDeviceGroupPeerMemoryFeatures(void*, u32, u32, u32, u32* pFeat) {
  if (pFeat) *pFeat = 0x7u;
}
u64 TRT_GUEST_ABI trt_vkGetBufferDeviceAddress(void*, const vk_buf_dev_addr_info* info) {
  return info ? (0x700000000000ull | (info->buffer << 16)) : 0;
}
u64 TRT_GUEST_ABI trt_vkGetBufferOpaqueCaptureAddress(void* d, const vk_buf_dev_addr_info* info) {
  return trt_vkGetBufferDeviceAddress(d, info);
}
u64 TRT_GUEST_ABI trt_vkGetDeviceMemoryOpaqueCaptureAddress(void*, const vk_buf_dev_addr_info* info) {
  return info ? (0x710000000000ull | (info->buffer << 16)) : 0;
}

i32 TRT_GUEST_ABI trt_vkCreateImage(void* dev_v, const vk_img_ci* ci, const void*, u64* pImg) {
  if (!ci || !pImg) return VK_ERROR_OUT_OF_DEVICE_MEMORY_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 id = d->next_h++;
  vk_img_obj im{};
  im.id = id;
  im.desc = {ci->imageType, ci->format, std::max(1u, ci->extent.width),
             std::max(1u, ci->extent.height), std::max(1u, ci->extent.depth),
             std::max(1u, ci->mipLevels), std::max(1u, ci->arrayLayers),
             ci->usage, ci->flags, 0, {}};
  d->imgs[id] = im;
  sync_img_obj(d, im);
  *pImg = id;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyImage(void* dev_v, u64 img, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->imgs.erase(img);
  if (g_vk_hooks_set && g_vk_hooks.destroy_image) g_vk_hooks.destroy_image(img, g_vk_hooks.user);
}
void TRT_GUEST_ABI trt_vkGetImageMemoryRequirements(void* dev_v, u64 img, vk_mem_reqs* mr) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 sz = 65536u;
  if (d && d->imgs.count(img)) {
    const auto& ds = d->imgs[img].desc;
    sz = (u64)ds.w * ds.h * std::max(1u, ds.depth) * std::max(1u, ds.layers) * 4ull;
  }
  if (mr) *mr = {((sz + 4095ull) & ~4095ull), 4096, 0x3u};
}
void TRT_GUEST_ABI trt_vkGetImageMemoryRequirements2(
    void* dev_v, const vk_img_mem_reqs_info2* info, vk_mem_reqs2* mr2) {
  if (info && mr2) trt_vkGetImageMemoryRequirements(dev_v, info->image, &mr2->memoryRequirements);
}
i32 TRT_GUEST_ABI trt_vkBindImageMemory(void* dev_v, u64 img, u64 mem, u64 off) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  if (d && d->imgs.count(img)) {
    d->imgs[img].mem_id = mem;
    d->imgs[img].mem_off = off;
  }
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkBindImageMemory2(void* dev_v, u32 cnt, const vk_bind_img_mem_info* infos) {
  for (u32 i = 0; i < cnt && infos; ++i) {
    trt_vkBindImageMemory(dev_v, infos[i].image, infos[i].memory, infos[i].memoryOffset);
  }
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkGetImageSubresourceLayout(
    void* dev_v, u64 img, const vk_img_subres* sr, vk_subres_layout* out) {
  if (!out) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u32 w = 64, h = 64, depth = 1, mip = sr ? sr->mipLevel : 0;
  if (d && d->imgs.count(img)) {
    w = std::max(1u, d->imgs[img].desc.w >> mip);
    h = std::max(1u, d->imgs[img].desc.h >> mip);
    depth = std::max(1u, d->imgs[img].desc.depth >> mip);
  }
  u64 row = (u64)w * 4ull;
  u64 slice = row * h;
  out->offset = 0;
  out->rowPitch = row;
  out->arrayPitch = slice;
  out->depthPitch = slice;
  out->size = slice * depth;
}
void TRT_GUEST_ABI trt_vkGetImageSparseMemoryRequirements(
    void*, u64, u32* pCount, vk_sparse_img_mem_reqs*) {
  if (pCount) *pCount = 0;
}
void TRT_GUEST_ABI trt_vkGetImageSparseMemoryRequirements2(
    void*, const void*, u32* pCount, vk_sparse_img_mem_reqs2*) {
  if (pCount) *pCount = 0;
}
i32 TRT_GUEST_ABI trt_vkQueueBindSparse(void*, u32, const void*, u64) { return VK_SUCCESS_V; }
i32 TRT_GUEST_ABI trt_vkCreateImageView(void* dev_v, const vk_img_view_ci* ci,
                                        const void*, u64* pView) {
  if (!ci || !pView) return VK_ERROR_OUT_OF_HOST_MEMORY_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 id = d->next_h++;
  d->views[id] = {id, ci->image, ci->format};
  *pView = id;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyImageView(void* dev_v, u64 v, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->views.erase(v);
}
i32 TRT_GUEST_ABI trt_vkCreateSampler(void* dev_v, const vk_sampler_ci* ci, const void*, u64* pS) {
  if (!pS) return VK_ERROR_OUT_OF_HOST_MEMORY_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 id = d->next_h++;
  vk_sampler_obj s{}; s.id = id;
  if (ci) { s.min_filter = ci->minFilter; s.mag_filter = ci->magFilter; s.addr_u = ci->addressModeU; s.addr_v = ci->addressModeV; }
  d->samps[id] = s;
  *pS = id;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroySampler(void* dev_v, u64 s, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->samps.erase(s);
}
i32 TRT_GUEST_ABI trt_vkCreateSamplerYcbcrConversion(void* dev_v, const void*, const void*, u64* pY) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (pY) *pY = dev_from(dev_v)->next_h++;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroySamplerYcbcrConversion(void*, u64, const void*) {}
void TRT_GUEST_ABI trt_vkGetDescriptorSetLayoutSupport(
    void*, const vk_desc_set_layout_ci*, vk_desc_set_layout_support* pSup) {
  if (pSup) pSup->supported = 1;
}
i32 TRT_GUEST_ABI trt_vkCreateDescriptorUpdateTemplate(
    void* dev_v, const vk_desc_tmpl_ci* ci, const void*, u64* pTmpl) {
  if (!ci || !pTmpl) return VK_ERROR_OUT_OF_HOST_MEMORY_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 id = d->next_h++;
  auto& vec = d->desc_templates[id];
  if (ci->pDescriptorUpdateEntries && ci->descriptorUpdateEntryCount > 0) {
    vec.assign(ci->pDescriptorUpdateEntries,
               ci->pDescriptorUpdateEntries + ci->descriptorUpdateEntryCount);
  }
  *pTmpl = id;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyDescriptorUpdateTemplate(void* dev_v, u64 tmpl, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->desc_templates.erase(tmpl);
}
void TRT_GUEST_ABI trt_vkUpdateDescriptorSetWithTemplate(
    void* dev_v, u64 dstSet, u64 tmpl, const void* pData) {
  if (!pData) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  if (!d || !d->dsets.count(dstSet) || !d->desc_templates.count(tmpl)) return;
  const u8* base = (const u8*)pData;
  for (const auto& te : d->desc_templates[tmpl]) {
    if (te.descriptorCount == 0) continue;
    auto& e = d->dsets[dstSet].entries[te.dstBinding];
    const u8* src = base + te.offset;
    if (te.descriptorType == 0 || te.descriptorType == 1 ||
        te.descriptorType == 2 || te.descriptorType == 3 || te.descriptorType == 10) {
      const auto* ii = (const vk_desc_img_info*)src;
      e.sampler = ii->sampler;
      e.img_view = ii->imageView;
    } else if (te.descriptorType == 6 || te.descriptorType == 7 ||
               te.descriptorType == 8 || te.descriptorType == 9) {
      const auto* bi = (const vk_desc_buf_info*)src;
      e.buf_id = bi->buffer;
      e.buf_off = bi->offset;
      e.buf_range = bi->range;
    }
  }
}

// ---- Shaders, Descriptors, Pipelines, RenderPasses -------------------------
i32 TRT_GUEST_ABI trt_vkCreateShaderModule(void* dev_v, const vk_shader_mod_ci* ci,
                                           const void*, u64* pMod) {
  if (!ci || !ci->pCode || ci->codeSize < 20 || !pMod) return VK_ERROR_INITIALIZATION_FAILED_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 id = d->next_h++;
  vk_shader_obj sh; sh.id = id;
  sh.spv.assign(ci->pCode, ci->pCode + (ci->codeSize / 4u));
  d->shaders[id] = std::move(sh);
  *pMod = id;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyShaderModule(void* dev_v, u64 m, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->shaders.erase(m);
}
i32 TRT_GUEST_ABI trt_vkCreatePipelineLayout(void* dev_v, const void*, const void*, u64* pOut) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (pOut) *pOut = dev_from(dev_v)->next_h++;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyPipelineLayout(void*, u64, const void*) {}
i32 TRT_GUEST_ABI trt_vkCreateDescriptorSetLayout(void* dev_v, const vk_desc_set_layout_ci*,
                                                 const void*, u64* pOut) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (pOut) *pOut = dev_from(dev_v)->next_h++;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyDescriptorSetLayout(void*, u64, const void*) {}
i32 TRT_GUEST_ABI trt_vkCreateDescriptorPool(void* dev_v, const void*, const void*, u64* pOut) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (pOut) *pOut = dev_from(dev_v)->next_h++;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyDescriptorPool(void*, u64, const void*) {}
i32 TRT_GUEST_ABI trt_vkResetDescriptorPool(void*, u64, u32) { return VK_SUCCESS_V; }
i32 TRT_GUEST_ABI trt_vkAllocateDescriptorSets(void* dev_v, const vk_desc_set_alloc_info* ai,
                                               u64* pSets) {
  if (!ai || !pSets) return VK_ERROR_OUT_OF_HOST_MEMORY_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  for (u32 i = 0; i < ai->descriptorSetCount; ++i) {
    u64 id = d->next_h++;
    d->dsets[id].id = id;
    pSets[i] = id;
  }
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkFreeDescriptorSets(void* dev_v, u64, u32 cnt, const u64* pSets) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  if (d && pSets) for (u32 i = 0; i < cnt; ++i) d->dsets.erase(pSets[i]);
  return VK_SUCCESS_V;
}
// Applies one VkWriteDescriptorSet to a binding map (a real descriptor
// set's entries, or a push-descriptor op's snapshot). Same limitation as
// the rest of the ICD: only element [0] of an array write is kept —
// descriptor arrays are a later batch.
static void apply_desc_write(std::map<u32, vk_desc_binding_val>& entries,
                             const vk_write_desc_set& w) {
  auto& e = entries[w.dstBinding];
  if (w.pBufferInfo && w.descriptorCount > 0) {
    e.buf_id = w.pBufferInfo[0].buffer;
    e.buf_off = w.pBufferInfo[0].offset;
    e.buf_range = w.pBufferInfo[0].range;
  }
  if (w.pImageInfo && w.descriptorCount > 0) {
    e.sampler = w.pImageInfo[0].sampler;
    e.img_view = w.pImageInfo[0].imageView;
  }
}

void TRT_GUEST_ABI trt_vkUpdateDescriptorSets(void* dev_v, u32 writeCnt,
                                              const vk_write_desc_set* writes, u32, const void*) {
  if (!writes) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  for (u32 i = 0; i < writeCnt; ++i) {
    const auto& w = writes[i];
    if (!d->dsets.count(w.dstSet)) continue;
    apply_desc_write(d->dsets[w.dstSet].entries, w);
  }
}

static void collect_spec(const vk_spec_info* si, std::vector<spirv_spec_entry>& out) {
  out.clear();
  if (!si || !si->pMapEntries || !si->pData) return;
  const u8* raw = (const u8*)si->pData;
  for (u32 i = 0; i < si->mapEntryCount; ++i) {
    const auto& me = si->pMapEntries[i];
    if (me.offset + 4 <= si->dataSize) {
      u32 v = 0; memcpy(&v, raw + me.offset, 4);
      out.push_back({me.constantID, v});
    }
  }
}

// Human-readable shader stage bit for failure logs.
static const char* vk_stage_name(u32 stage) {
  switch (stage) {
    case 0x1u: return "VERTEX";
    case 0x2u: return "TESSELLATION_CONTROL";
    case 0x4u: return "TESSELLATION_EVALUATION";
    case 0x8u: return "GEOMETRY";
    case 0x10u: return "FRAGMENT";
    case 0x20u: return "COMPUTE";
    default: return "UNKNOWN";
  }
}

i32 TRT_GUEST_ABI trt_vkCreateGraphicsPipelines(void* dev_v, u64, u32 count,
                                                 const vk_gfx_pipe_ci* cis, const void*, u64* pPipes) {
  if (!cis || !pPipes) return VK_ERROR_INITIALIZATION_FAILED_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  bool any_failed = false;
  for (u32 i = 0; i < count; ++i) {
    const auto& ci = cis[i];
    u64 pid = d->next_h++;
    vk_pipe_obj po{}; po.id = pid;
    std::string vs_h, ps_h;
    std::vector<spirv_spec_entry> specs;
    bool stage_failed = false;

    for (u32 s = 0; s < ci.stageCount; ++s) {
      const auto& st = ci.pStages[s];
      if (!d->shaders.count(st.module)) {
        // Fail-visible: a stage whose module does not exist used to be
        // silently skipped, producing a pipeline with a missing stage.
        vk_logf("vulkan: vkCreateGraphicsPipelines FAILED - pipeline %u stage %u (%s): "
                "shader module %llu does not exist",
                i, s, vk_stage_name(st.stage), (unsigned long long)st.module);
        stage_failed = true;
        break;
      }
      collect_spec(st.pSpecializationInfo, specs);
      const auto& spv = d->shaders[st.module].spv;
      spirv_stage sst = (st.stage == 0x10u) ? SPV_STAGE_FRAGMENT : SPV_STAGE_VERTEX;
      const spirv_compile_result* cached = spirv_cache_lookup(
          spv.data(), (u32)spv.size(), sst, st.pName,
          specs.empty() ? nullptr : specs.data(), (u32)specs.size());
      spirv_compile_result cr;
      if (cached) {
        cr = *cached;
      } else {
        cr = spirv_to_hlsl(
          spv.data(), (u32)spv.size(), sst, st.pName,
          specs.empty() ? nullptr : specs.data(), (u32)specs.size());
        if (cr.ok)
          spirv_cache_store(spv.data(), (u32)spv.size(), sst, st.pName,
                            specs.empty() ? nullptr : specs.data(),
                            (u32)specs.size(), cr);
      }
      if (!cr.ok) {
        // Fail-visible: never create a pipeline with a silently missing
        // stage — the game must see the failure, not render black.
        vk_logf("vulkan: vkCreateGraphicsPipelines FAILED - pipeline %u stage %u (%s, entry '%s'): "
                "SPIR-V->HLSL failed (%u words, fnv %016llx): %.200s",
                i, s, vk_stage_name(st.stage), st.pName ? st.pName : "(null)",
                (u32)spv.size(),
                (unsigned long long)vkshader_cache::fnv1a(spv.data(), (u32)spv.size()),
                cr.log.c_str());
        stage_failed = true;
        break;
      }
      if (sst == SPV_STAGE_VERTEX) vs_h = cr.hlsl;
      else ps_h = cr.hlsl;
      if (cr.push_constant_bytes) po.push_reg = cr.push_constant_reg;
      for (const auto& b : cr.bindings) {
        po.bindings.push_back({b.set, b.binding, b.kind, st.stage, b.hlsl_reg, b.smp_reg});
      }
    }

    if (stage_failed) {
      pPipes[i] = 0;  // VK_NULL_HANDLE: this pipeline was not created
      any_failed = true;
      continue;
    }

    std::vector<trt_vk_vert_bind> vbs;
    std::vector<trt_vk_vert_attr> vas;
    if (ci.pVertexInputState) {
      for (u32 j = 0; j < ci.pVertexInputState->vertexBindingDescriptionCount; ++j) {
        const auto& b = ci.pVertexInputState->pVertexBindingDescriptions[j];
        vbs.push_back({b.binding, b.stride, b.inputRate});
      }
      for (u32 j = 0; j < ci.pVertexInputState->vertexAttributeDescriptionCount; ++j) {
        const auto& a = ci.pVertexInputState->pVertexAttributeDescriptions[j];
        vas.push_back({a.location, a.binding, a.format, a.offset});
      }
    }
    if (ci.pViewportState && ci.pViewportState->viewportCount > 0 && ci.pViewportState->pViewports) {
      po.init_vp = ci.pViewportState->pViewports[0];
      po.has_init_vp = true;
    }
    if (ci.pViewportState && ci.pViewportState->scissorCount > 0 && ci.pViewportState->pScissors) {
      po.init_sc = ci.pViewportState->pScissors[0];
    }

    trt_vk_pipeline_desc pd{};
    pd.pipeline_id = pid;
    pd.vs_hlsl = vs_h.empty() ? nullptr : vs_h.c_str();
    pd.vs_len = (u32)vs_h.size();
    pd.ps_hlsl = ps_h.empty() ? nullptr : ps_h.c_str();
    pd.ps_len = (u32)ps_h.size();
    pd.vtx_binds = vbs.empty() ? nullptr : vbs.data();
    pd.vtx_bind_count = (u32)vbs.size();
    pd.vtx_attrs = vas.empty() ? nullptr : vas.data();
    pd.vtx_attr_count = (u32)vas.size();
    pd.topology = ci.pInputAssemblyState ? ci.pInputAssemblyState->topology : 3u;
    pd.polygon_mode = ci.pRasterizationState ? ci.pRasterizationState->polygonMode : 0u;
    pd.cull_mode = ci.pRasterizationState ? ci.pRasterizationState->cullMode : 0u;
    pd.front_face = ci.pRasterizationState ? ci.pRasterizationState->frontFace : 0u;
    if (ci.pDepthStencilState) {
      pd.depth_test = ci.pDepthStencilState->depthTestEnable ? 1 : 0;
      pd.depth_write = ci.pDepthStencilState->depthWriteEnable ? 1 : 0;
      pd.depth_op = ci.pDepthStencilState->depthCompareOp;
    }
    if (ci.pColorBlendState && ci.pColorBlendState->attachmentCount > 0 &&
        ci.pColorBlendState->pAttachments) {
      const auto& a = ci.pColorBlendState->pAttachments[0];
      pd.blend_en = a.blendEnable ? 1 : 0;
      pd.src_rgb = a.srcColorBlendFactor; pd.dst_rgb = a.dstColorBlendFactor; pd.op_rgb = a.colorBlendOp;
      pd.src_a = a.srcAlphaBlendFactor; pd.dst_a = a.dstAlphaBlendFactor; pd.op_a = a.alphaBlendOp;
      pd.color_mask = (u8)(a.colorWriteMask & 0xFu);
      memcpy(po.blend_const, ci.pColorBlendState->blendConstants, 16);
    } else {
      pd.color_mask = 0xFu;
    }
    d->pipes[pid] = std::move(po);
    if (g_vk_hooks_set && g_vk_hooks.create_pipeline) {
      g_vk_hooks.create_pipeline(&pd, g_vk_hooks.user);
    }
    pPipes[i] = pid;
  }
  // Vulkan semantics: pipelines that failed are VK_NULL_HANDLE in pPipes
  // and the call reports an error; the ones that succeeded stay valid.
  return any_failed ? VK_ERROR_INITIALIZATION_FAILED_V : VK_SUCCESS_V;
}

i32 TRT_GUEST_ABI trt_vkCreateComputePipelines(void* dev_v, u64, u32 count,
                                                const vk_comp_pipe_ci* cis, const void*, u64* pPipes) {
  if (!cis || !pPipes) return VK_ERROR_INITIALIZATION_FAILED_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  bool any_failed = false;
  for (u32 i = 0; i < count; ++i) {
    u64 pid = d->next_h++;
    vk_pipe_obj po{}; po.id = pid; po.is_compute = true;
    std::string cs_h;
    bool stage_failed = false;
    if (!d->shaders.count(cis[i].stage.module)) {
      // Fail-visible: same missing-stage rule as graphics pipelines.
      vk_logf("vulkan: vkCreateComputePipelines FAILED - pipeline %u (COMPUTE): "
              "shader module %llu does not exist",
              i, (unsigned long long)cis[i].stage.module);
      stage_failed = true;
    } else {
      std::vector<spirv_spec_entry> specs;
      collect_spec(cis[i].stage.pSpecializationInfo, specs);
      const auto& spv = d->shaders[cis[i].stage.module].spv;
      const spirv_compile_result* cached = spirv_cache_lookup(
          spv.data(), (u32)spv.size(), SPV_STAGE_COMPUTE,
          cis[i].stage.pName, specs.empty() ? nullptr : specs.data(),
          (u32)specs.size());
      spirv_compile_result cr;
      if (cached) {
        cr = *cached;
      } else {
        cr = spirv_to_hlsl(
          spv.data(), (u32)spv.size(), SPV_STAGE_COMPUTE, cis[i].stage.pName,
          specs.empty() ? nullptr : specs.data(), (u32)specs.size());
        if (cr.ok)
          spirv_cache_store(spv.data(), (u32)spv.size(), SPV_STAGE_COMPUTE,
                            cis[i].stage.pName,
                            specs.empty() ? nullptr : specs.data(),
                            (u32)specs.size(), cr);
      }
      if (!cr.ok) {
        // Fail-visible: never create a compute pipeline with a silently
        // missing shader — the game must see the failure.
        vk_logf("vulkan: vkCreateComputePipelines FAILED - pipeline %u (COMPUTE, entry '%s'): "
                "SPIR-V->HLSL failed (%u words, fnv %016llx): %.200s",
                i, cis[i].stage.pName ? cis[i].stage.pName : "(null)",
                (u32)spv.size(),
                (unsigned long long)vkshader_cache::fnv1a(spv.data(), (u32)spv.size()),
                cr.log.c_str());
        stage_failed = true;
      } else {
        cs_h = cr.hlsl;
        if (cr.push_constant_bytes) po.push_reg = cr.push_constant_reg;
        for (const auto& b : cr.bindings) {
          po.bindings.push_back({b.set, b.binding, b.kind, 0x20u, b.hlsl_reg, b.smp_reg});
        }
      }
    }
    if (stage_failed) {
      pPipes[i] = 0;  // VK_NULL_HANDLE: this pipeline was not created
      any_failed = true;
      continue;
    }
    d->pipes[pid] = std::move(po);
    trt_vk_pipeline_desc pd{};
    pd.pipeline_id = pid; pd.is_compute = 1;
    pd.cs_hlsl = cs_h.empty() ? nullptr : cs_h.c_str();
    pd.cs_len = (u32)cs_h.size();
    if (g_vk_hooks_set && g_vk_hooks.create_pipeline) g_vk_hooks.create_pipeline(&pd, g_vk_hooks.user);
    pPipes[i] = pid;
  }
  return any_failed ? VK_ERROR_INITIALIZATION_FAILED_V : VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyPipeline(void* dev_v, u64 p, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->pipes.erase(p);
  if (g_vk_hooks_set && g_vk_hooks.destroy_pipeline) g_vk_hooks.destroy_pipeline(p, g_vk_hooks.user);
}
i32 TRT_GUEST_ABI trt_vkCreatePipelineCache(
    void* dev_v, const vk_pipe_cache_ci* ci, const void*, u64* pCache) {
  if (!pCache) return VK_ERROR_OUT_OF_HOST_MEMORY_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 id = d->next_h++;
  auto& buf = d->pipe_caches[id];
  if (ci && ci->pInitialData && ci->initialDataSize >= 32) {
    const u8* raw = (const u8*)ci->pInitialData;
    buf.assign(raw, raw + ci->initialDataSize);
  } else {
    buf.assign(32, 0);
    u32 hdr[4] = {32u, 1u, 0x1002u, 0x73DFu};
    memcpy(buf.data(), hdr, sizeof(hdr));
  }
  *pCache = id;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyPipelineCache(void* dev_v, u64 pc, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->pipe_caches.erase(pc);
}
i32 TRT_GUEST_ABI trt_vkGetPipelineCacheData(void* dev_v, u64 pc, size_t* pSize, void* pData) {
  if (!pSize) return VK_ERROR_INITIALIZATION_FAILED_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  static const u8 k_def[32] = {32, 0, 0, 0, 1, 0, 0, 0, 0x02, 0x10, 0, 0, 0xDF, 0x73, 0, 0};
  const u8* src = k_def;
  size_t len = sizeof(k_def);
  if (d && d->pipe_caches.count(pc) && !d->pipe_caches[pc].empty()) {
    src = d->pipe_caches[pc].data();
    len = d->pipe_caches[pc].size();
  }
  if (!pData) { *pSize = len; return VK_SUCCESS_V; }
  size_t cp = std::min(*pSize, len);
  memcpy(pData, src, cp);
  *pSize = cp;
  return (cp < len) ? VK_INCOMPLETE_V : VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkMergePipelineCaches(void*, u64, u32, const u64*) { return VK_SUCCESS_V; }

template <typename Ci>
static i32 create_rp_common(void* dev_v, const Ci* ci, u64* pRp) {
  if (!ci || !pRp) return VK_ERROR_INITIALIZATION_FAILED_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 id = d->next_h++;
  vk_rp_obj rp{}; rp.id = id;
  for (u32 i = 0; i < ci->attachmentCount; ++i) rp.att_load_op.push_back(ci->pAttachments[i].loadOp);
  if (ci->subpassCount > 0 && ci->pSubpasses) {
    const auto& sp = ci->pSubpasses[0];
    for (u32 c = 0; c < sp.colorAttachmentCount; ++c) {
      rp.color_atts.push_back(sp.pColorAttachments[c].attachment);
    }
    if (sp.pDepthStencilAttachment) rp.depth_att = (int)sp.pDepthStencilAttachment->attachment;
  }
  d->rps[id] = std::move(rp);
  *pRp = id;
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkCreateRenderPass(void* dev_v, const vk_rp_ci* ci, const void*, u64* pRp) {
  return create_rp_common(dev_v, ci, pRp);
}
i32 TRT_GUEST_ABI trt_vkCreateRenderPass2(void* dev_v, const vk_rp_ci2* ci, const void*, u64* pRp) {
  return create_rp_common(dev_v, ci, pRp);
}
void TRT_GUEST_ABI trt_vkGetRenderAreaGranularity(void*, u64, vk_extent2d* pGran) {
  if (pGran) *pGran = {1u, 1u};
}
void TRT_GUEST_ABI trt_vkDestroyRenderPass(void* dev_v, u64 rp, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->rps.erase(rp);
}
i32 TRT_GUEST_ABI trt_vkCreateFramebuffer(void* dev_v, const vk_fb_ci* ci, const void*, u64* pFb) {
  if (!ci || !pFb) return VK_ERROR_INITIALIZATION_FAILED_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 id = d->next_h++;
  vk_fb_obj fb{id, ci->width, ci->height, {}};
  for (u32 i = 0; i < ci->attachmentCount; ++i) fb.view_ids.push_back(ci->pAttachments[i]);
  d->fbs[id] = std::move(fb);
  *pFb = id;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyFramebuffer(void* dev_v, u64 fb, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->fbs.erase(fb);
}

// ---- Sync Objects & Queries ------------------------------------------------
i32 TRT_GUEST_ABI trt_vkCreateFence(void* dev_v, const u32* ci, const void*, u64* pF) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 id = d->next_h++;
  d->fences[id] = ci ? ((ci[4] & 1u) != 0) : true; // sType/pNext/flags at word 4
  if (pF) *pF = id;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyFence(void* dev_v, u64 f, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->fences.erase(f);
}
i32 TRT_GUEST_ABI trt_vkResetFences(void*, u32, const u64*) { return VK_SUCCESS_V; }
i32 TRT_GUEST_ABI trt_vkWaitForFences(void*, u32, const u64*, u32, u64) { return VK_SUCCESS_V; }
i32 TRT_GUEST_ABI trt_vkGetFenceStatus(void*, u64) { return VK_SUCCESS_V; }
i32 TRT_GUEST_ABI trt_vkCreateSemaphore(void* dev_v, const vk_sem_type_ci* ci, const void*, u64* pS) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 id = d->next_h++;
  u64 init_val = 0;
  for (const vk_sem_type_ci* p = ci ? (const vk_sem_type_ci*)ci->pNext : nullptr; p;
       p = (const vk_sem_type_ci*)p->pNext) {
    if (p->sType == 1000207002u /*VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO*/) {
      init_val = p->initialValue;
      break;
    }
  }
  d->semaphores[id] = init_val;
  if (pS) *pS = id;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroySemaphore(void* dev_v, u64 s, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->semaphores.erase(s);
}
i32 TRT_GUEST_ABI trt_vkGetSemaphoreCounterValue(void* dev_v, u64 s, u64* pVal) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  if (pVal) *pVal = (d && d->semaphores.count(s)) ? d->semaphores[s] : 0;
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkWaitSemaphores(void*, const vk_sem_wait_info*, u64) { return VK_SUCCESS_V; }
i32 TRT_GUEST_ABI trt_vkSignalSemaphore(void* dev_v, const vk_sem_signal_info* si) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v); d && si) d->semaphores[si->semaphore] = si->value;
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkCreateEvent(void* dev_v, const void*, const void*, u64* pE) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  u64 id = d->next_h++;
  d->events[id] = false;
  if (pE) *pE = id;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyEvent(void* dev_v, u64 e, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->events.erase(e);
}
i32 TRT_GUEST_ABI trt_vkGetEventStatus(void* dev_v, u64 e) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  return (d && d->events.count(e) && d->events[e]) ? VK_EVENT_SET_V : VK_EVENT_RESET_V;
}
i32 TRT_GUEST_ABI trt_vkSetEvent(void* dev_v, u64 e) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->events[e] = true;
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkResetEvent(void* dev_v, u64 e) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_device_obj* d = dev_from(dev_v)) d->events[e] = false;
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkCreateQueryPool(void* dev_v, const void*, const void*, u64* pQ) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (pQ) *pQ = dev_from(dev_v)->next_h++;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyQueryPool(void*, u64, const void*) {}
void TRT_GUEST_ABI trt_vkResetQueryPool(void*, u64, u32, u32) {}
i32 TRT_GUEST_ABI trt_vkGetQueryPoolResults(void*, u64, u32, u32, size_t ds, void* pD, u64, u32) {
  if (pD && ds) memset(pD, 0, ds);
  return VK_SUCCESS_V;
}

// ---- Command Pools, Buffers, Recording, Submit -----------------------------
i32 TRT_GUEST_ABI trt_vkCreateCommandPool(void* dev_v, const vk_cmd_pool_ci*, const void*, u64* pCp) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (pCp) *pCp = dev_from(dev_v)->next_h++;
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkDestroyCommandPool(void*, u64, const void*) {}
i32 TRT_GUEST_ABI trt_vkResetCommandPool(void*, u64, u32) { return VK_SUCCESS_V; }
void TRT_GUEST_ABI trt_vkTrimCommandPool(void*, u64, u32) {}
i32 TRT_GUEST_ABI trt_vkAllocateCommandBuffers(void* dev_v, const vk_cmd_buf_alloc* ai,
                                               void** pCBs) {
  if (!ai || !pCBs) return VK_ERROR_OUT_OF_HOST_MEMORY_V;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  for (u32 i = 0; i < ai->commandBufferCount; ++i) {
    vk_cmdbuf_obj* cb = new vk_cmdbuf_obj();
    cb->device = d;
    d->cmdbufs.push_back(cb);
    pCBs[i] = cb;
  }
  return VK_SUCCESS_V;
}
void TRT_GUEST_ABI trt_vkFreeCommandBuffers(void* dev_v, u64, u32 cnt, void* const* pCBs) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_device_obj* d = dev_from(dev_v);
  if (!d || !pCBs) return;
  for (u32 i = 0; i < cnt; ++i) {
    vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)pCBs[i];
    d->cmdbufs.erase(std::remove(d->cmdbufs.begin(), d->cmdbufs.end(), cb), d->cmdbufs.end());
    delete cb;
  }
}
i32 TRT_GUEST_ABI trt_vkBeginCommandBuffer(void* cb_v, const void*) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) { cb->ops.clear(); cb->recording = true; }
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkEndCommandBuffer(void* cb_v) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) cb->recording = false;
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkResetCommandBuffer(void* cb_v, u32) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) cb->ops.clear();
  return VK_SUCCESS_V;
}

void TRT_GUEST_ABI trt_vkCmdBeginRenderPass(void* cb_v, const vk_rp_begin_info* bi, u32) {
  if (!cb_v || !bi) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v;
  vk_device_obj* d = dev_from(cb->device);
  vk_cmd_op op{}; op.kind = vk_cmd_op::OP_BEGIN_RP;
  op.rp.area_x = bi->renderArea.offset.x; op.rp.area_y = bi->renderArea.offset.y;
  op.rp.area_w = bi->renderArea.extent.width; op.rp.area_h = bi->renderArea.extent.height;
  op.rp.depth_val = 1.0f;
  if (d && d->rps.count(bi->renderPass) && d->fbs.count(bi->framebuffer)) {
    const auto& rp = d->rps[bi->renderPass];
    const auto& fb = d->fbs[bi->framebuffer];
    for (u32 c = 0; c < (u32)rp.color_atts.size() && c < 8u; ++c) {
      u32 att = rp.color_atts[c];
      if (att < fb.view_ids.size() && d->views.count(fb.view_ids[att])) {
        op.rp.color_imgs[c] = d->views[fb.view_ids[att]].image_id;
      }
      if (att < rp.att_load_op.size() && rp.att_load_op[att] == 1u /*CLEAR*/) {
        op.rp.clear_color_mask |= (1u << c);
        if (bi->pClearValues && att < bi->clearValueCount) {
          memcpy(op.rp.clear_rgba[c], bi->pClearValues[att].f, 16);
        }
      }
      op.rp.color_count = c + 1;
    }
    if (rp.depth_att >= 0 && (size_t)rp.depth_att < fb.view_ids.size()) {
      u64 dv = fb.view_ids[(size_t)rp.depth_att];
      if (d->views.count(dv)) op.rp.depth_img = d->views[dv].image_id;
      if ((size_t)rp.depth_att < rp.att_load_op.size() && rp.att_load_op[(size_t)rp.depth_att] == 1u) {
        op.rp.clear_depth = 1;
        if (bi->pClearValues && (u32)rp.depth_att < bi->clearValueCount) {
          const auto& cv = bi->pClearValues[rp.depth_att];
          op.rp.depth_val = cv.f[0];
          memcpy(&op.rp.stencil_val, &cv.f[1], 4);
        }
      }
    }
  }
  cb->ops.push_back(op);
}
void TRT_GUEST_ABI trt_vkCmdEndRenderPass(void* cb_v) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_END_RP; cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdBeginRendering(void* cb_v, const vk_rendering_info* ri) {
  if (!cb_v || !ri) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v;
  vk_device_obj* d = dev_from(cb->device);
  vk_cmd_op op{}; op.kind = vk_cmd_op::OP_BEGIN_RENDERING;
  op.rp.area_x = ri->renderArea.offset.x; op.rp.area_y = ri->renderArea.offset.y;
  op.rp.area_w = ri->renderArea.extent.width; op.rp.area_h = ri->renderArea.extent.height;
  op.rp.depth_val = 1.0f;
  for (u32 c = 0; c < ri->colorAttachmentCount && c < 8u; ++c) {
    const auto& att = ri->pColorAttachments[c];
    if (d && d->views.count(att.imageView)) op.rp.color_imgs[c] = d->views[att.imageView].image_id;
    if (att.loadOp == 1u) {
      op.rp.clear_color_mask |= (1u << c);
      memcpy(op.rp.clear_rgba[c], att.clearValue.f, 16);
    }
    op.rp.color_count = c + 1;
  }
  if (ri->pDepthAttachment && d && d->views.count(ri->pDepthAttachment->imageView)) {
    op.rp.depth_img = d->views[ri->pDepthAttachment->imageView].image_id;
    if (ri->pDepthAttachment->loadOp == 1u) {
      op.rp.clear_depth = 1;
      op.rp.depth_val = ri->pDepthAttachment->clearValue.f[0];
    }
  }
  cb->ops.push_back(op);
}
void TRT_GUEST_ABI trt_vkCmdEndRendering(void* cb_v) { trt_vkCmdEndRenderPass(cb_v); }
void TRT_GUEST_ABI trt_vkCmdNextSubpass(void*, u32) {}
void TRT_GUEST_ABI trt_vkCmdBeginRenderPass2(void* cb_v, const vk_rp_begin_info* bi, const void*) {
  trt_vkCmdBeginRenderPass(cb_v, bi, 0);
}
void TRT_GUEST_ABI trt_vkCmdEndRenderPass2(void* cb_v, const void*) {
  trt_vkCmdEndRenderPass(cb_v);
}
void TRT_GUEST_ABI trt_vkCmdNextSubpass2(void*, const void*, const void*) {}
void TRT_GUEST_ABI trt_vkCmdBindPipeline(void* cb_v, u32 bp, u64 p) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_BIND_PIPE; op.bind_point = bp; op.handle = p;
    cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdSetViewport(void* cb_v, u32, u32 cnt, const vk_viewport* vp) {
  if (!cb_v || !vp || !cnt) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_cmd_op op{}; op.kind = vk_cmd_op::OP_SET_VP; op.vp = vp[0];
  ((vk_cmdbuf_obj*)cb_v)->ops.push_back(op);
}
void TRT_GUEST_ABI trt_vkCmdSetScissor(void* cb_v, u32, u32 cnt, const vk_rect2d* sc) {
  if (!cb_v || !sc || !cnt) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_cmd_op op{}; op.kind = vk_cmd_op::OP_SET_SC; op.sc = sc[0];
  ((vk_cmdbuf_obj*)cb_v)->ops.push_back(op);
}
void TRT_GUEST_ABI trt_vkCmdBindVertexBuffers(void* cb_v, u32 first, u32 cnt,
                                              const u64* bufs, const u64* offs) {
  if (!cb_v || !bufs || !cnt) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_cmd_op op{}; op.kind = vk_cmd_op::OP_BIND_VB;
  op.first_slot = first; op.slot_count = std::min(cnt, 8u);
  for (u32 i = 0; i < op.slot_count; ++i) {
    op.vbs[i] = bufs[i]; op.vb_offs[i] = offs ? offs[i] : 0;
  }
  ((vk_cmdbuf_obj*)cb_v)->ops.push_back(op);
}
void TRT_GUEST_ABI trt_vkCmdBindIndexBuffer(void* cb_v, u64 buf, u64 off, u32 itype) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_BIND_IB; op.handle = buf;
    op.vb_offs[0] = off; op.idx_u32 = (itype == 1u) ? 1 : 0;
    cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdBindDescriptorSets(void* cb_v, u32 bp, u64, u32 first,
                                               u32 cnt, const u64* sets, u32, const u32*) {
  if (!cb_v || !sets || !cnt) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_cmd_op op{}; op.kind = vk_cmd_op::OP_BIND_DS; op.bind_point = bp;
  op.ds_first = first; op.ds_count = std::min(cnt, 8u);
  for (u32 i = 0; i < op.ds_count; ++i) op.dsets[i] = sets[i];
  ((vk_cmdbuf_obj*)cb_v)->ops.push_back(op);
}
// VK_KHR_push_descriptor: the writes are snapshotted into the command
// buffer op (same apply_desc_write machinery as vkUpdateDescriptorSets)
// and resolved at draw/dispatch time in place of a bound set at the same
// index. The layout handle is unused — the ICD does not record layouts,
// and the writes carry their own binding info.
void TRT_GUEST_ABI trt_vkCmdPushDescriptorSetKHR(void* cb_v, u32 bp, u64,
                                                 u32 set, u32 writeCount,
                                                 const vk_write_desc_set* writes) {
  if (!cb_v || !writes || !writeCount || set >= 8u) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_cmd_op op{}; op.kind = vk_cmd_op::OP_PUSH_DS; op.bind_point = bp;
  op.ds_first = set;
  for (u32 i = 0; i < writeCount; ++i) {
    apply_desc_write(op.push_entries, writes[i]);
  }
  ((vk_cmdbuf_obj*)cb_v)->ops.push_back(std::move(op));
}
void TRT_GUEST_ABI trt_vkCmdPushConstants(void* cb_v, u64, u32, u32 off, u32 sz, const void* pVals) {
  if (!cb_v || !pVals || off + sz > 256u) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_cmd_op op{}; op.kind = vk_cmd_op::OP_PUSH_CONST; op.push_off = off; op.push_sz = sz;
  memcpy(op.push_bytes, pVals, sz);
  ((vk_cmdbuf_obj*)cb_v)->ops.push_back(op);
}
void TRT_GUEST_ABI trt_vkCmdDraw(void* cb_v, u32 vc, u32 ic, u32 fv, u32 fi) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_DRAW; op.a0 = vc; op.a1 = ic; op.a2 = fv; op.a3 = fi;
    cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdDrawIndexed(void* cb_v, u32 ic, u32 inst, u32 fi, i32 vo, u32 finst) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_DRAW_IDX;
    op.a0 = ic; op.a1 = inst; op.a2 = fi; op.vtx_off = vo; op.a3 = finst;
    cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdDispatch(void* cb_v, u32 x, u32 y, u32 z) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_DISPATCH; op.a0 = x; op.a1 = y; op.a2 = z;
    cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdCopyBuffer(void* cb_v, u64 src, u64 dst, u32 cnt, const vk_buf_copy* regs) {
  if (!cb_v || !regs) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v;
  for (u32 i = 0; i < cnt; ++i) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_COPY_BUF; op.handle = src; op.dst_handle = dst;
    op.vb_offs[0] = regs[i].srcOffset; op.vb_offs[1] = regs[i].dstOffset; op.vb_offs[2] = regs[i].size;
    cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdCopyBufferToImage(void* cb_v, u64 src, u64 dst, u32, u32 cnt,
                                              const vk_buf_img_copy* regs) {
  if (!cb_v || !regs) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v;
  for (u32 i = 0; i < cnt; ++i) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_COPY_BUF_IMG; op.handle = src; op.dst_handle = dst;
    op.bic = regs[i];
    cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdPipelineBarrier(void*, u32, u32, u32, u32, const void*, u32, const void*, u32, const void*) {}
void TRT_GUEST_ABI trt_vkCmdPipelineBarrier2(void*, const void*) {}
void TRT_GUEST_ABI trt_vkCmdClearColorImage(void*, u64 img, u32, const void*, u32, const void*) {
  static bool s_logged = false;
  if (!s_logged) { s_logged = true; vk_logf("vulkan: vkCmdClearColorImage(img=%llu)", (unsigned long long)img); }
}
void TRT_GUEST_ABI trt_vkCmdBlitImage(void*, u64 src, u32, u64 dst, u32, u32, const void*, u32) {
  static bool s_logged = false;
  if (!s_logged) { s_logged = true; vk_logf("vulkan: vkCmdBlitImage(src=%llu -> dst=%llu)", (unsigned long long)src, (unsigned long long)dst); }
}
void TRT_GUEST_ABI trt_vkCmdCopyImage(void*, u64 src, u32, u64 dst, u32, u32, const void*) {
  static bool s_logged = false;
  if (!s_logged) { s_logged = true; vk_logf("vulkan: vkCmdCopyImage(src=%llu -> dst=%llu)", (unsigned long long)src, (unsigned long long)dst); }
}
void TRT_GUEST_ABI trt_vkCmdCopyImageToBuffer(void*, u64, u32, u64, u32, const void*) {}
void TRT_GUEST_ABI trt_vkCmdSetBlendConstants(void*, const float*) {}
void TRT_GUEST_ABI trt_vkCmdSetDepthBias(void*, float, float, float) {}
void TRT_GUEST_ABI trt_vkCmdSetLineWidth(void*, float) {}
void TRT_GUEST_ABI trt_vkCmdSetDepthBounds(void*, float, float) {}
void TRT_GUEST_ABI trt_vkCmdSetStencilCompareMask(void*, u32, u32) {}
void TRT_GUEST_ABI trt_vkCmdSetStencilWriteMask(void*, u32, u32) {}
void TRT_GUEST_ABI trt_vkCmdSetStencilReference(void*, u32, u32) {}
void TRT_GUEST_ABI trt_vkCmdSetDeviceMask(void*, u32) {}
void TRT_GUEST_ABI trt_vkCmdClearDepthStencilImage(void*, u64, u32, const void*, u32, const void*) {}
void TRT_GUEST_ABI trt_vkCmdClearAttachments(void*, u32, const void*, u32, const void*) {}
void TRT_GUEST_ABI trt_vkCmdResolveImage(void*, u64, u32, u64, u32, u32, const void*) {}
void TRT_GUEST_ABI trt_vkCmdSetEvent(void* cb_v, u64 ev, u32) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_SET_EVENT; op.handle = ev; cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdResetEvent(void* cb_v, u64 ev, u32) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_RESET_EVENT; op.handle = ev; cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdWaitEvents(
    void*, u32, const u64*, u32, u32, u32, const void*, u32, const void*, u32, const void*) {}
void TRT_GUEST_ABI trt_vkCmdSetEvent2(void* cb_v, u64 ev, const void*) {
  trt_vkCmdSetEvent(cb_v, ev, 0);
}
void TRT_GUEST_ABI trt_vkCmdResetEvent2(void* cb_v, u64 ev, u64) {
  trt_vkCmdResetEvent(cb_v, ev, 0);
}
void TRT_GUEST_ABI trt_vkCmdWaitEvents2(void*, u32, const u64*, const void*) {}
void TRT_GUEST_ABI trt_vkCmdBeginQuery(void*, u64, u32, u32) {}
void TRT_GUEST_ABI trt_vkCmdEndQuery(void*, u64, u32) {}
void TRT_GUEST_ABI trt_vkCmdResetQueryPoolCmd(void*, u64, u32, u32) {}
void TRT_GUEST_ABI trt_vkCmdWriteTimestamp(void*, u32, u64, u32) {}
void TRT_GUEST_ABI trt_vkCmdWriteTimestamp2(void*, u64, u64, u32) {}
void TRT_GUEST_ABI trt_vkCmdCopyQueryPoolResults(
    void* cb_v, u64, u32, u32 count, u64 dstBuf, u64 dstOff, u64 stride, u32 flags) {
  if (!cb_v || !dstBuf || !count) return;
  const u64 elem = ((flags & 1u) ? 8ull : 4ull) * ((flags & 4u) ? 2ull : 1ull);
  const u64 total = stride ? ((u64)count * stride) : ((u64)count * elem);
  if (total == 0 || total > 65536u) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_FILL_BUF;
    op.handle = dstBuf; op.vb_offs[0] = dstOff; op.vb_offs[1] = total; op.a0 = 0;
    cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdDrawIndirect(void* cb_v, u64 buf, u64 off, u32 cnt, u32 stride) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_DRAW_INDIRECT;
    op.handle = buf; op.vb_offs[0] = off; op.a0 = cnt; op.a1 = stride;
    cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdDrawIndexedIndirect(void* cb_v, u64 buf, u64 off, u32 cnt, u32 stride) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_DRAW_IDX_INDIRECT;
    op.handle = buf; op.vb_offs[0] = off; op.a0 = cnt; op.a1 = stride;
    cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdDrawIndirectCount(
    void* cb_v, u64 buf, u64 off, u64 cntBuf, u64 cntOff, u32 maxCnt, u32 stride) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_DRAW_INDIRECT_COUNT;
    op.handle = buf; op.dst_handle = cntBuf; op.vb_offs[0] = off; op.vb_offs[1] = cntOff;
    op.a0 = maxCnt; op.a1 = stride;
    cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdDrawIndexedIndirectCount(
    void* cb_v, u64 buf, u64 off, u64 cntBuf, u64 cntOff, u32 maxCnt, u32 stride) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_DRAW_IDX_INDIRECT_COUNT;
    op.handle = buf; op.dst_handle = cntBuf; op.vb_offs[0] = off; op.vb_offs[1] = cntOff;
    op.a0 = maxCnt; op.a1 = stride;
    cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdDispatchIndirect(void* cb_v, u64 buf, u64 off) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_DISPATCH_INDIRECT;
    op.handle = buf; op.vb_offs[0] = off;
    cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdDispatchBase(void* cb_v, u32, u32, u32, u32 x, u32 y, u32 z) {
  trt_vkCmdDispatch(cb_v, x, y, z);
}
void TRT_GUEST_ABI trt_vkCmdUpdateBuffer(void* cb_v, u64 dst, u64 off, u64 sz, const void* data) {
  if (!cb_v || !data || !sz) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_UPDATE_BUF;
    op.handle = dst; op.vb_offs[0] = off;
    op.inline_bytes.assign((const u8*)data, (const u8*)data + (size_t)sz);
    cb->ops.push_back(std::move(op));
  }
}
void TRT_GUEST_ABI trt_vkCmdFillBuffer(void* cb_v, u64 dst, u64 off, u64 sz, u32 data) {
  if (!cb_v || !sz) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_FILL_BUF;
    op.handle = dst; op.vb_offs[0] = off; op.vb_offs[1] = sz; op.a0 = data;
    cb->ops.push_back(op);
  }
}
void TRT_GUEST_ABI trt_vkCmdExecuteCommands(void* cb_v, u32 cnt, void* const* pCBs) {
  if (!cb_v || !pCBs || !cnt) return;
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)cb_v) {
    vk_cmd_op op{}; op.kind = vk_cmd_op::OP_EXEC_CMDS;
    op.sec_cbs.assign(pCBs, pCBs + cnt);
    cb->ops.push_back(std::move(op));
  }
}

i32 TRT_GUEST_ABI trt_vkQueueSubmit(void* q_v, u32 cnt, const vk_submit_info* subs, u64) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_queue_obj* q = (vk_queue_obj*)q_v;
  vk_device_obj* d = dev_from(q ? q->device : nullptr);
  if (!d) return VK_SUCCESS_V;
  for (u32 i = 0; i < cnt && subs; ++i) {
    for (u32 c = 0; c < subs[i].commandBufferCount && subs[i].pCommandBuffers; ++c) {
      if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)subs[i].pCommandBuffers[c]) {
        exec_cmdbuf(d, cb);
      }
    }
  }
  return VK_SUCCESS_V;
}
i32 TRT_GUEST_ABI trt_vkQueueSubmit2(void* q_v, u32 cnt, const vk_submit_info2* subs, u64) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  vk_queue_obj* q = (vk_queue_obj*)q_v;
  vk_device_obj* d = dev_from(q ? q->device : nullptr);
  if (!d) return VK_SUCCESS_V;
  for (u32 i = 0; i < cnt && subs; ++i) {
    for (u32 c = 0; c < subs[i].commandBufferInfoCount && subs[i].pCommandBufferInfos; ++c) {
      if (vk_cmdbuf_obj* cb = (vk_cmdbuf_obj*)subs[i].pCommandBufferInfos[c].commandBuffer) {
        exec_cmdbuf(d, cb);
      }
    }
  }
  return VK_SUCCESS_V;
}

void* TRT_GUEST_ABI trt_vkGetInstanceProcAddr(void*, const char* name) {
  if (!name) return nullptr;
  for (size_t i = 0; i < k_vk_proc_count; ++i) {
    if (strcmp(k_vk_procs[i].name, name) == 0) return k_vk_procs[i].fn;
  }
  return nullptr;
}
void* TRT_GUEST_ABI trt_vkGetDeviceProcAddr(void*, const char* name) {
  return trt_vkGetInstanceProcAddr(nullptr, name);
}

const vk_proc_entry k_vk_procs[] = {
    {"vkGetInstanceProcAddr", (void*)&trt_vkGetInstanceProcAddr},
    {"vkGetDeviceProcAddr", (void*)&trt_vkGetDeviceProcAddr},
    {"vkEnumerateInstanceVersion", (void*)&trt_vkEnumerateInstanceVersion},
    {"vkEnumerateInstanceExtensionProperties", (void*)&trt_vkEnumerateInstanceExtensionProperties},
    {"vkEnumerateInstanceLayerProperties", (void*)&trt_vkEnumerateInstanceLayerProperties},
    {"vkCreateInstance", (void*)&trt_vkCreateInstance},
    {"vkDestroyInstance", (void*)&trt_vkDestroyInstance},
    {"vkEnumeratePhysicalDevices", (void*)&trt_vkEnumeratePhysicalDevices},
    {"vkGetPhysicalDeviceProperties", (void*)&trt_vkGetPhysicalDeviceProperties},
    {"vkGetPhysicalDeviceProperties2", (void*)&trt_vkGetPhysicalDeviceProperties2},
    {"vkGetPhysicalDeviceProperties2KHR", (void*)&trt_vkGetPhysicalDeviceProperties2},
    {"vkGetPhysicalDeviceFeatures", (void*)&trt_vkGetPhysicalDeviceFeatures},
    {"vkGetPhysicalDeviceFeatures2", (void*)&trt_vkGetPhysicalDeviceFeatures2},
    {"vkGetPhysicalDeviceFeatures2KHR", (void*)&trt_vkGetPhysicalDeviceFeatures2},
    {"vkGetPhysicalDeviceFormatProperties", (void*)&trt_vkGetPhysicalDeviceFormatProperties},
    {"vkGetPhysicalDeviceFormatProperties2", (void*)&trt_vkGetPhysicalDeviceFormatProperties2},
    {"vkGetPhysicalDeviceFormatProperties2KHR", (void*)&trt_vkGetPhysicalDeviceFormatProperties2},
    {"vkGetPhysicalDeviceImageFormatProperties", (void*)&trt_vkGetPhysicalDeviceImageFormatProperties},
    {"vkGetPhysicalDeviceImageFormatProperties2", (void*)&trt_vkGetPhysicalDeviceImageFormatProperties2},
    {"vkGetPhysicalDeviceImageFormatProperties2KHR", (void*)&trt_vkGetPhysicalDeviceImageFormatProperties2},
    {"vkGetPhysicalDeviceQueueFamilyProperties", (void*)&trt_vkGetPhysicalDeviceQueueFamilyProperties},
    {"vkGetPhysicalDeviceQueueFamilyProperties2", (void*)&trt_vkGetPhysicalDeviceQueueFamilyProperties2},
    {"vkGetPhysicalDeviceQueueFamilyProperties2KHR", (void*)&trt_vkGetPhysicalDeviceQueueFamilyProperties2},
    {"vkGetPhysicalDeviceMemoryProperties", (void*)&trt_vkGetPhysicalDeviceMemoryProperties},
    {"vkGetPhysicalDeviceMemoryProperties2", (void*)&trt_vkGetPhysicalDeviceMemoryProperties2},
    {"vkGetPhysicalDeviceMemoryProperties2KHR", (void*)&trt_vkGetPhysicalDeviceMemoryProperties2},
    {"vkGetPhysicalDeviceSparseImageFormatProperties", (void*)&trt_vkGetPhysicalDeviceSparseImageFormatProperties},
    {"vkGetPhysicalDeviceSparseImageFormatProperties2", (void*)&trt_vkGetPhysicalDeviceSparseImageFormatProperties2},
    {"vkGetPhysicalDeviceSparseImageFormatProperties2KHR", (void*)&trt_vkGetPhysicalDeviceSparseImageFormatProperties2},
    {"vkGetPhysicalDeviceExternalBufferProperties", (void*)&trt_vkGetPhysicalDeviceExternalBufferProperties},
    {"vkGetPhysicalDeviceExternalBufferPropertiesKHR", (void*)&trt_vkGetPhysicalDeviceExternalBufferProperties},
    {"vkGetPhysicalDeviceExternalSemaphoreProperties", (void*)&trt_vkGetPhysicalDeviceExternalSemaphoreProperties},
    {"vkGetPhysicalDeviceExternalSemaphorePropertiesKHR", (void*)&trt_vkGetPhysicalDeviceExternalSemaphoreProperties},
    {"vkGetPhysicalDeviceExternalFenceProperties", (void*)&trt_vkGetPhysicalDeviceExternalFenceProperties},
    {"vkGetPhysicalDeviceExternalFencePropertiesKHR", (void*)&trt_vkGetPhysicalDeviceExternalFenceProperties},
    {"vkEnumeratePhysicalDeviceGroups", (void*)&trt_vkEnumeratePhysicalDeviceGroups},
    {"vkEnumeratePhysicalDeviceGroupsKHR", (void*)&trt_vkEnumeratePhysicalDeviceGroups},
    {"vkEnumerateDeviceExtensionProperties", (void*)&trt_vkEnumerateDeviceExtensionProperties},
    {"vkEnumerateDeviceLayerProperties", (void*)&trt_vkEnumerateDeviceLayerProperties},

    {"vkCreateWin32SurfaceKHR", (void*)&trt_vkCreateWin32SurfaceKHR},
    {"vkDestroySurfaceKHR", (void*)&trt_vkDestroySurfaceKHR},
    {"vkGetPhysicalDeviceWin32PresentationSupportKHR", (void*)&trt_vkGetPhysicalDeviceWin32PresentationSupportKHR},
    {"vkGetPhysicalDeviceSurfaceSupportKHR", (void*)&trt_vkGetPhysicalDeviceSurfaceSupportKHR},
    {"vkGetPhysicalDeviceSurfaceCapabilitiesKHR", (void*)&trt_vkGetPhysicalDeviceSurfaceCapabilitiesKHR},
    {"vkGetPhysicalDeviceSurfaceFormatsKHR", (void*)&trt_vkGetPhysicalDeviceSurfaceFormatsKHR},
    {"vkGetPhysicalDeviceSurfacePresentModesKHR", (void*)&trt_vkGetPhysicalDeviceSurfacePresentModesKHR},

    {"vkCreateDevice", (void*)&trt_vkCreateDevice},
    {"vkDestroyDevice", (void*)&trt_vkDestroyDevice},
    {"vkGetDeviceQueue", (void*)&trt_vkGetDeviceQueue},
    {"vkGetDeviceQueue2", (void*)&trt_vkGetDeviceQueue2},
    {"vkDeviceWaitIdle", (void*)&trt_vkDeviceWaitIdle},
    {"vkQueueWaitIdle", (void*)&trt_vkQueueWaitIdle},
    {"vkCreateSwapchainKHR", (void*)&trt_vkCreateSwapchainKHR},
    {"vkDestroySwapchainKHR", (void*)&trt_vkDestroySwapchainKHR},
    {"vkGetSwapchainImagesKHR", (void*)&trt_vkGetSwapchainImagesKHR},
    {"vkAcquireNextImageKHR", (void*)&trt_vkAcquireNextImageKHR},
    {"vkQueuePresentKHR", (void*)&trt_vkQueuePresentKHR},

    {"vkAllocateMemory", (void*)&trt_vkAllocateMemory},
    {"vkFreeMemory", (void*)&trt_vkFreeMemory},
    {"vkMapMemory", (void*)&trt_vkMapMemory},
    {"vkUnmapMemory", (void*)&trt_vkUnmapMemory},
    {"vkFlushMappedMemoryRanges", (void*)&trt_vkFlushMappedMemoryRanges},
    {"vkInvalidateMappedMemoryRanges", (void*)&trt_vkInvalidateMappedMemoryRanges},
    {"vkCreateBuffer", (void*)&trt_vkCreateBuffer},
    {"vkDestroyBuffer", (void*)&trt_vkDestroyBuffer},
    {"vkGetBufferMemoryRequirements", (void*)&trt_vkGetBufferMemoryRequirements},
    {"vkGetBufferMemoryRequirements2", (void*)&trt_vkGetBufferMemoryRequirements2},
    {"vkGetBufferMemoryRequirements2KHR", (void*)&trt_vkGetBufferMemoryRequirements2},
    {"vkBindBufferMemory", (void*)&trt_vkBindBufferMemory},
    {"vkBindBufferMemory2", (void*)&trt_vkBindBufferMemory2},
    {"vkBindBufferMemory2KHR", (void*)&trt_vkBindBufferMemory2},
    {"vkCreateBufferView", (void*)&trt_vkCreateBufferView},
    {"vkDestroyBufferView", (void*)&trt_vkDestroyBufferView},
    {"vkGetDeviceMemoryCommitment", (void*)&trt_vkGetDeviceMemoryCommitment},
    {"vkGetDeviceGroupPeerMemoryFeatures", (void*)&trt_vkGetDeviceGroupPeerMemoryFeatures},
    {"vkGetDeviceGroupPeerMemoryFeaturesKHR", (void*)&trt_vkGetDeviceGroupPeerMemoryFeatures},
    {"vkGetBufferDeviceAddress", (void*)&trt_vkGetBufferDeviceAddress},
    {"vkGetBufferDeviceAddressKHR", (void*)&trt_vkGetBufferDeviceAddress},
    {"vkGetBufferOpaqueCaptureAddress", (void*)&trt_vkGetBufferOpaqueCaptureAddress},
    {"vkGetBufferOpaqueCaptureAddressKHR", (void*)&trt_vkGetBufferOpaqueCaptureAddress},
    {"vkGetDeviceMemoryOpaqueCaptureAddress", (void*)&trt_vkGetDeviceMemoryOpaqueCaptureAddress},
    {"vkGetDeviceMemoryOpaqueCaptureAddressKHR", (void*)&trt_vkGetDeviceMemoryOpaqueCaptureAddress},
    {"vkCreateImage", (void*)&trt_vkCreateImage},
    {"vkDestroyImage", (void*)&trt_vkDestroyImage},
    {"vkGetImageMemoryRequirements", (void*)&trt_vkGetImageMemoryRequirements},
    {"vkGetImageMemoryRequirements2", (void*)&trt_vkGetImageMemoryRequirements2},
    {"vkGetImageMemoryRequirements2KHR", (void*)&trt_vkGetImageMemoryRequirements2},
    {"vkBindImageMemory", (void*)&trt_vkBindImageMemory},
    {"vkBindImageMemory2", (void*)&trt_vkBindImageMemory2},
    {"vkBindImageMemory2KHR", (void*)&trt_vkBindImageMemory2},
    {"vkGetImageSubresourceLayout", (void*)&trt_vkGetImageSubresourceLayout},
    {"vkGetImageSparseMemoryRequirements", (void*)&trt_vkGetImageSparseMemoryRequirements},
    {"vkGetImageSparseMemoryRequirements2", (void*)&trt_vkGetImageSparseMemoryRequirements2},
    {"vkGetImageSparseMemoryRequirements2KHR", (void*)&trt_vkGetImageSparseMemoryRequirements2},
    {"vkQueueBindSparse", (void*)&trt_vkQueueBindSparse},
    {"vkCreateImageView", (void*)&trt_vkCreateImageView},
    {"vkDestroyImageView", (void*)&trt_vkDestroyImageView},
    {"vkCreateSampler", (void*)&trt_vkCreateSampler},
    {"vkDestroySampler", (void*)&trt_vkDestroySampler},
    {"vkCreateSamplerYcbcrConversion", (void*)&trt_vkCreateSamplerYcbcrConversion},
    {"vkCreateSamplerYcbcrConversionKHR", (void*)&trt_vkCreateSamplerYcbcrConversion},
    {"vkDestroySamplerYcbcrConversion", (void*)&trt_vkDestroySamplerYcbcrConversion},
    {"vkDestroySamplerYcbcrConversionKHR", (void*)&trt_vkDestroySamplerYcbcrConversion},

    {"vkCreateShaderModule", (void*)&trt_vkCreateShaderModule},
    {"vkDestroyShaderModule", (void*)&trt_vkDestroyShaderModule},
    {"vkCreatePipelineCache", (void*)&trt_vkCreatePipelineCache},
    {"vkDestroyPipelineCache", (void*)&trt_vkDestroyPipelineCache},
    {"vkGetPipelineCacheData", (void*)&trt_vkGetPipelineCacheData},
    {"vkMergePipelineCaches", (void*)&trt_vkMergePipelineCaches},
    {"vkCreatePipelineLayout", (void*)&trt_vkCreatePipelineLayout},
    {"vkDestroyPipelineLayout", (void*)&trt_vkDestroyPipelineLayout},
    {"vkCreateDescriptorSetLayout", (void*)&trt_vkCreateDescriptorSetLayout},
    {"vkDestroyDescriptorSetLayout", (void*)&trt_vkDestroyDescriptorSetLayout},
    {"vkGetDescriptorSetLayoutSupport", (void*)&trt_vkGetDescriptorSetLayoutSupport},
    {"vkGetDescriptorSetLayoutSupportKHR", (void*)&trt_vkGetDescriptorSetLayoutSupport},
    {"vkCreateDescriptorPool", (void*)&trt_vkCreateDescriptorPool},
    {"vkDestroyDescriptorPool", (void*)&trt_vkDestroyDescriptorPool},
    {"vkResetDescriptorPool", (void*)&trt_vkResetDescriptorPool},
    {"vkAllocateDescriptorSets", (void*)&trt_vkAllocateDescriptorSets},
    {"vkFreeDescriptorSets", (void*)&trt_vkFreeDescriptorSets},
    {"vkUpdateDescriptorSets", (void*)&trt_vkUpdateDescriptorSets},
    {"vkCreateDescriptorUpdateTemplate", (void*)&trt_vkCreateDescriptorUpdateTemplate},
    {"vkCreateDescriptorUpdateTemplateKHR", (void*)&trt_vkCreateDescriptorUpdateTemplate},
    {"vkDestroyDescriptorUpdateTemplate", (void*)&trt_vkDestroyDescriptorUpdateTemplate},
    {"vkDestroyDescriptorUpdateTemplateKHR", (void*)&trt_vkDestroyDescriptorUpdateTemplate},
    {"vkUpdateDescriptorSetWithTemplate", (void*)&trt_vkUpdateDescriptorSetWithTemplate},
    {"vkUpdateDescriptorSetWithTemplateKHR", (void*)&trt_vkUpdateDescriptorSetWithTemplate},
    {"vkCreateGraphicsPipelines", (void*)&trt_vkCreateGraphicsPipelines},
    {"vkCreateComputePipelines", (void*)&trt_vkCreateComputePipelines},
    {"vkDestroyPipeline", (void*)&trt_vkDestroyPipeline},
    {"vkCreateRenderPass", (void*)&trt_vkCreateRenderPass},
    {"vkCreateRenderPass2", (void*)&trt_vkCreateRenderPass2},
    {"vkCreateRenderPass2KHR", (void*)&trt_vkCreateRenderPass2},
    {"vkDestroyRenderPass", (void*)&trt_vkDestroyRenderPass},
    {"vkGetRenderAreaGranularity", (void*)&trt_vkGetRenderAreaGranularity},
    {"vkCreateFramebuffer", (void*)&trt_vkCreateFramebuffer},
    {"vkDestroyFramebuffer", (void*)&trt_vkDestroyFramebuffer},

    {"vkCreateFence", (void*)&trt_vkCreateFence},
    {"vkDestroyFence", (void*)&trt_vkDestroyFence},
    {"vkResetFences", (void*)&trt_vkResetFences},
    {"vkWaitForFences", (void*)&trt_vkWaitForFences},
    {"vkGetFenceStatus", (void*)&trt_vkGetFenceStatus},
    {"vkCreateSemaphore", (void*)&trt_vkCreateSemaphore},
    {"vkDestroySemaphore", (void*)&trt_vkDestroySemaphore},
    {"vkGetSemaphoreCounterValue", (void*)&trt_vkGetSemaphoreCounterValue},
    {"vkGetSemaphoreCounterValueKHR", (void*)&trt_vkGetSemaphoreCounterValue},
    {"vkWaitSemaphores", (void*)&trt_vkWaitSemaphores},
    {"vkWaitSemaphoresKHR", (void*)&trt_vkWaitSemaphores},
    {"vkSignalSemaphore", (void*)&trt_vkSignalSemaphore},
    {"vkSignalSemaphoreKHR", (void*)&trt_vkSignalSemaphore},
    {"vkCreateEvent", (void*)&trt_vkCreateEvent},
    {"vkDestroyEvent", (void*)&trt_vkDestroyEvent},
    {"vkGetEventStatus", (void*)&trt_vkGetEventStatus},
    {"vkSetEvent", (void*)&trt_vkSetEvent},
    {"vkResetEvent", (void*)&trt_vkResetEvent},
    {"vkCreateQueryPool", (void*)&trt_vkCreateQueryPool},
    {"vkDestroyQueryPool", (void*)&trt_vkDestroyQueryPool},
    {"vkResetQueryPool", (void*)&trt_vkResetQueryPool},
    {"vkGetQueryPoolResults", (void*)&trt_vkGetQueryPoolResults},

    {"vkCreateCommandPool", (void*)&trt_vkCreateCommandPool},
    {"vkDestroyCommandPool", (void*)&trt_vkDestroyCommandPool},
    {"vkResetCommandPool", (void*)&trt_vkResetCommandPool},
    {"vkTrimCommandPool", (void*)&trt_vkTrimCommandPool},
    {"vkTrimCommandPoolKHR", (void*)&trt_vkTrimCommandPool},
    {"vkAllocateCommandBuffers", (void*)&trt_vkAllocateCommandBuffers},
    {"vkFreeCommandBuffers", (void*)&trt_vkFreeCommandBuffers},
    {"vkBeginCommandBuffer", (void*)&trt_vkBeginCommandBuffer},
    {"vkEndCommandBuffer", (void*)&trt_vkEndCommandBuffer},
    {"vkResetCommandBuffer", (void*)&trt_vkResetCommandBuffer},
    {"vkCmdBeginRenderPass", (void*)&trt_vkCmdBeginRenderPass},
    {"vkCmdEndRenderPass", (void*)&trt_vkCmdEndRenderPass},
    {"vkCmdBeginRenderPass2", (void*)&trt_vkCmdBeginRenderPass2},
    {"vkCmdBeginRenderPass2KHR", (void*)&trt_vkCmdBeginRenderPass2},
    {"vkCmdEndRenderPass2", (void*)&trt_vkCmdEndRenderPass2},
    {"vkCmdEndRenderPass2KHR", (void*)&trt_vkCmdEndRenderPass2},
    {"vkCmdBeginRendering", (void*)&trt_vkCmdBeginRendering},
    {"vkCmdBeginRenderingKHR", (void*)&trt_vkCmdBeginRendering},
    {"vkCmdEndRendering", (void*)&trt_vkCmdEndRendering},
    {"vkCmdEndRenderingKHR", (void*)&trt_vkCmdEndRendering},
    {"vkCmdNextSubpass", (void*)&trt_vkCmdNextSubpass},
    {"vkCmdNextSubpass2", (void*)&trt_vkCmdNextSubpass2},
    {"vkCmdNextSubpass2KHR", (void*)&trt_vkCmdNextSubpass2},
    {"vkCmdBindPipeline", (void*)&trt_vkCmdBindPipeline},
    {"vkCmdSetViewport", (void*)&trt_vkCmdSetViewport},
    {"vkCmdSetScissor", (void*)&trt_vkCmdSetScissor},
    {"vkCmdBindVertexBuffers", (void*)&trt_vkCmdBindVertexBuffers},
    {"vkCmdBindIndexBuffer", (void*)&trt_vkCmdBindIndexBuffer},
    {"vkCmdBindDescriptorSets", (void*)&trt_vkCmdBindDescriptorSets},
    {"vkCmdPushDescriptorSetKHR", (void*)&trt_vkCmdPushDescriptorSetKHR},
    {"vkCmdPushConstants", (void*)&trt_vkCmdPushConstants},
    {"vkCmdDraw", (void*)&trt_vkCmdDraw},
    {"vkCmdDrawIndexed", (void*)&trt_vkCmdDrawIndexed},
    {"vkCmdDrawIndirect", (void*)&trt_vkCmdDrawIndirect},
    {"vkCmdDrawIndexedIndirect", (void*)&trt_vkCmdDrawIndexedIndirect},
    {"vkCmdDrawIndirectCount", (void*)&trt_vkCmdDrawIndirectCount},
    {"vkCmdDrawIndirectCountKHR", (void*)&trt_vkCmdDrawIndirectCount},
    {"vkCmdDrawIndexedIndirectCount", (void*)&trt_vkCmdDrawIndexedIndirectCount},
    {"vkCmdDrawIndexedIndirectCountKHR", (void*)&trt_vkCmdDrawIndexedIndirectCount},
    {"vkCmdDispatch", (void*)&trt_vkCmdDispatch},
    {"vkCmdDispatchIndirect", (void*)&trt_vkCmdDispatchIndirect},
    {"vkCmdDispatchBase", (void*)&trt_vkCmdDispatchBase},
    {"vkCmdDispatchBaseKHR", (void*)&trt_vkCmdDispatchBase},
    {"vkCmdCopyBuffer", (void*)&trt_vkCmdCopyBuffer},
    {"vkCmdCopyBufferToImage", (void*)&trt_vkCmdCopyBufferToImage},
    {"vkCmdCopyImageToBuffer", (void*)&trt_vkCmdCopyImageToBuffer},
    {"vkCmdCopyImage", (void*)&trt_vkCmdCopyImage},
    {"vkCmdBlitImage", (void*)&trt_vkCmdBlitImage},
    {"vkCmdUpdateBuffer", (void*)&trt_vkCmdUpdateBuffer},
    {"vkCmdFillBuffer", (void*)&trt_vkCmdFillBuffer},
    {"vkCmdClearColorImage", (void*)&trt_vkCmdClearColorImage},
    {"vkCmdClearDepthStencilImage", (void*)&trt_vkCmdClearDepthStencilImage},
    {"vkCmdClearAttachments", (void*)&trt_vkCmdClearAttachments},
    {"vkCmdResolveImage", (void*)&trt_vkCmdResolveImage},
    {"vkCmdPipelineBarrier", (void*)&trt_vkCmdPipelineBarrier},
    {"vkCmdPipelineBarrier2", (void*)&trt_vkCmdPipelineBarrier2},
    {"vkCmdPipelineBarrier2KHR", (void*)&trt_vkCmdPipelineBarrier2},
    {"vkCmdSetEvent", (void*)&trt_vkCmdSetEvent},
    {"vkCmdResetEvent", (void*)&trt_vkCmdResetEvent},
    {"vkCmdWaitEvents", (void*)&trt_vkCmdWaitEvents},
    {"vkCmdSetEvent2", (void*)&trt_vkCmdSetEvent2},
    {"vkCmdSetEvent2KHR", (void*)&trt_vkCmdSetEvent2},
    {"vkCmdResetEvent2", (void*)&trt_vkCmdResetEvent2},
    {"vkCmdResetEvent2KHR", (void*)&trt_vkCmdResetEvent2},
    {"vkCmdWaitEvents2", (void*)&trt_vkCmdWaitEvents2},
    {"vkCmdWaitEvents2KHR", (void*)&trt_vkCmdWaitEvents2},
    {"vkCmdBeginQuery", (void*)&trt_vkCmdBeginQuery},
    {"vkCmdEndQuery", (void*)&trt_vkCmdEndQuery},
    {"vkCmdResetQueryPool", (void*)&trt_vkCmdResetQueryPoolCmd},
    {"vkCmdWriteTimestamp", (void*)&trt_vkCmdWriteTimestamp},
    {"vkCmdWriteTimestamp2", (void*)&trt_vkCmdWriteTimestamp2},
    {"vkCmdWriteTimestamp2KHR", (void*)&trt_vkCmdWriteTimestamp2},
    {"vkCmdCopyQueryPoolResults", (void*)&trt_vkCmdCopyQueryPoolResults},
    {"vkCmdExecuteCommands", (void*)&trt_vkCmdExecuteCommands},
    {"vkCmdSetBlendConstants", (void*)&trt_vkCmdSetBlendConstants},
    {"vkCmdSetDepthBias", (void*)&trt_vkCmdSetDepthBias},
    {"vkCmdSetLineWidth", (void*)&trt_vkCmdSetLineWidth},
    {"vkCmdSetDepthBounds", (void*)&trt_vkCmdSetDepthBounds},
    {"vkCmdSetStencilCompareMask", (void*)&trt_vkCmdSetStencilCompareMask},
    {"vkCmdSetStencilWriteMask", (void*)&trt_vkCmdSetStencilWriteMask},
    {"vkCmdSetStencilReference", (void*)&trt_vkCmdSetStencilReference},
    {"vkCmdSetDeviceMask", (void*)&trt_vkCmdSetDeviceMask},
    {"vkCmdSetDeviceMaskKHR", (void*)&trt_vkCmdSetDeviceMask},
    {"vkQueueSubmit", (void*)&trt_vkQueueSubmit},
    {"vkQueueSubmit2", (void*)&trt_vkQueueSubmit2},
    {"vkQueueSubmit2KHR", (void*)&trt_vkQueueSubmit2},
};

const size_t k_vk_proc_count = sizeof(k_vk_procs) / sizeof(k_vk_procs[0]);

}  // namespace

extern "C" {

void trt_vk_set_hooks(const trt_vk_hooks* hooks) {
  std::lock_guard<std::mutex> lk(g_vk_mu);
  if (hooks) { g_vk_hooks = *hooks; g_vk_hooks_set = true; }
  else { memset(&g_vk_hooks, 0, sizeof(g_vk_hooks)); g_vk_hooks_set = false; }
}

int trt_bridge_register_vulkan(void) {
  static std::vector<trt_named_fn> s_reg;
  if (s_reg.empty()) {
    s_reg.reserve(k_vk_proc_count);
    for (size_t i = 0; i < k_vk_proc_count; ++i) {
      s_reg.push_back({k_vk_procs[i].name, k_vk_procs[i].fn});
    }
  }
  trt_module_register("vulkan-1.dll", TRT_ROUTE_BUILTIN, s_reg.data(),
                      (int)s_reg.size());
  return 0;
}

}  // extern "C"
