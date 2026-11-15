// tiramisu/src/guest/guest_vulkan.c
// PE64 guest executable exercising the real Vulkan 1.0-1.3 + WSI bridge
// through IAT imports from vulkan-1.dll: instance -> GPU -> Win32 surface ->
// device -> swapchain -> VB + UBO + SPIR-V shaders -> descriptor set ->
// render pass -> graphics pipeline -> command buffer -> draw -> submit -> present.
// Returns bitmask (0x7FFF when all stages succeed).
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
typedef unsigned long long u64;

#ifdef _WIN32
#define VAPI __stdcall
#else
#define VAPI
#endif

int _fltused = 1;

static void zclear(void* p, u64 bytes) {
  volatile u8* d = (volatile u8*)p;
  for (u64 i = 0; i < bytes; ++i) d[i] = 0;
}

typedef struct { u32 width, height; } vk_ext2d;
typedef struct { i32 x, y; } vk_off2d;
typedef struct { vk_off2d offset; vk_ext2d extent; } vk_rect2d;
typedef struct { float x, y, width, height, minDepth, maxDepth; } vk_vp;

typedef struct {
  u32 sType; const void* pNext; u32 flags;
  const void* pApplicationInfo;
  u32 enabledLayerCount; const char* const* ppEnabledLayerNames;
  u32 enabledExtensionCount; const char* const* ppEnabledExtensionNames;
} vk_inst_ci;

typedef struct {
  u32 apiVersion, driverVersion, vendorID, deviceID, deviceType;
  char deviceName[256]; u8 pipelineCacheUUID[16];
  u32 raw[160];
} vk_pd_props;

typedef struct { u32 queueFlags, queueCount, timestampValidBits, g[3]; } vk_qf_props;
typedef struct { u32 memTypeCount; u32 types[64]; u32 heapCount; u64 heaps[32]; } vk_mem_props;

typedef struct {
  u32 sType; const void* pNext; u32 flags; u32 queueFamilyIndex;
  u32 queueCount; const float* pQueuePriorities;
} vk_dq_ci;

typedef struct {
  u32 sType; const void* pNext; u32 flags;
  u32 queueCreateInfoCount; const vk_dq_ci* pQueueCreateInfos;
  u32 enabledLayerCount; const char* const* ppEnabledLayerNames;
  u32 enabledExtensionCount; const char* const* ppEnabledExtensionNames;
  const void* pEnabledFeatures;
} vk_dev_ci;

typedef struct {
  u32 sType; const void* pNext; u32 flags; void* hinstance; void* hwnd;
} vk_win32_surf_ci;

typedef struct {
  u32 sType; const void* pNext; u32 flags; u64 surface; u32 minImageCount;
  u32 imageFormat; u32 imageColorSpace; vk_ext2d imageExtent; u32 imageArrayLayers;
  u32 imageUsage; u32 imageSharingMode; u32 queueFamilyIndexCount;
  const u32* pQueueFamilyIndices; u32 preTransform; u32 compositeAlpha;
  u32 presentMode; u32 clipped; u64 oldSwapchain;
} vk_sc_ci;

typedef struct {
  u32 sType; const void* pNext; u32 waitSemaphoreCount; const u64* pWaitSemaphores;
  u32 swapchainCount; const u64* pSwapchains; const u32* pImageIndices; i32* pResults;
} vk_present_info;

typedef struct { u32 sType; const void* pNext; u64 allocationSize; u32 memoryTypeIndex; } vk_alloc_info;
typedef struct { u64 size, alignment; u32 memoryTypeBits; } vk_mem_reqs;
typedef struct {
  u32 sType; const void* pNext; u32 flags; u64 size; u32 usage; u32 sharingMode;
  u32 qCount; const u32* pQ;
} vk_buf_ci;

typedef struct {
  u32 sType; const void* pNext; u32 flags; u64 image; u32 viewType; u32 format;
  u32 components[4]; u32 subres[5];
} vk_iv_ci;

typedef struct { u32 sType; const void* pNext; u32 flags; u64 codeSize; const u32* pCode; } vk_sm_ci;

typedef struct { u32 binding, descriptorType, descriptorCount, stageFlags; const u64* pImm; } vk_dsl_bind;
typedef struct { u32 sType; const void* pNext; u32 flags; u32 bindingCount; const vk_dsl_bind* pBindings; } vk_dsl_ci;
typedef struct { u32 stageFlags, offset, size; } vk_pc_range;
typedef struct {
  u32 sType; const void* pNext; u32 flags; u32 setLayoutCount; const u64* pSetLayouts;
  u32 pushConstantRangeCount; const vk_pc_range* pPushConstantRanges;
} vk_pl_ci;

typedef struct { u32 type, descriptorCount; } vk_dp_size;
typedef struct { u32 sType; const void* pNext; u32 flags; u32 maxSets; u32 poolSizeCount; const vk_dp_size* pPoolSizes; } vk_dp_ci;
typedef struct { u32 sType; const void* pNext; u64 pool; u32 count; const u64* pSetLayouts; } vk_ds_ai;
typedef struct { u64 buffer, offset, range; } vk_db_info;
typedef struct {
  u32 sType; const void* pNext; u64 dstSet; u32 dstBinding; u32 dstArrayElement;
  u32 descriptorCount; u32 descriptorType; const void* pImageInfo;
  const vk_db_info* pBufferInfo; const u64* pTexelBufferView;
} vk_write_ds;

typedef struct { u32 flags, format, samples, loadOp, storeOp, sLoad, sStore, initLayout, finalLayout; } vk_att_desc;
typedef struct { u32 attachment, layout; } vk_att_ref;
typedef struct {
  u32 flags, bindPoint, inCnt; const vk_att_ref* pIn; u32 colCnt; const vk_att_ref* pCol;
  const vk_att_ref* pRes; const vk_att_ref* pDs; u32 presCnt; const u32* pPres;
} vk_subpass;
typedef struct {
  u32 sType; const void* pNext; u32 flags; u32 attachmentCount; const vk_att_desc* pAttachments;
  u32 subpassCount; const vk_subpass* pSubpasses; u32 depCount; const void* pDeps;
} vk_rp_ci;
typedef struct {
  u32 sType; const void* pNext; u32 flags; u64 renderPass; u32 attCnt; const u64* pAtts;
  u32 width, height, layers;
} vk_fb_ci;

typedef struct {
  u32 sType; const void* pNext; u32 flags; u32 stage; u64 module; const char* pName;
  const void* pSpec;
} vk_pipe_stage;
typedef struct { u32 binding, stride, inputRate; } vk_vb_desc;
typedef struct { u32 location, binding, format, offset; } vk_va_desc;
typedef struct {
  u32 sType; const void* pNext; u32 flags;
  u32 vbCnt; const vk_vb_desc* pVbs; u32 vaCnt; const vk_va_desc* pVas;
} vk_pipe_vi;
typedef struct { u32 sType; const void* pNext; u32 flags; u32 topology; u32 restart; } vk_pipe_ia;
typedef struct { u32 sType; const void* pNext; u32 flags; u32 vpCnt; const vk_vp* pVps; u32 scCnt; const vk_rect2d* pScs; } vk_pipe_vp;
typedef struct {
  u32 sType; const void* pNext; u32 flags; u32 clamp, discard, polyMode, cullMode, frontFace, bias;
  float bC, bCl, bS, lineW;
} vk_pipe_rs;
typedef struct { u32 blendEn, sRGB, dRGB, opRGB, sA, dA, opA, mask; } vk_cb_att;
typedef struct { u32 sType; const void* pNext; u32 flags; u32 logicEn, logicOp, attCnt; const vk_cb_att* pAtts; float bc[4]; } vk_pipe_cb;
typedef struct {
  u32 sType; const void* pNext; u32 flags; u32 stageCount; const vk_pipe_stage* pStages;
  const vk_pipe_vi* pVi; const vk_pipe_ia* pIa; const void* pTess;
  const vk_pipe_vp* pVp; const vk_pipe_rs* pRs; const void* pMs; const void* pDs;
  const vk_pipe_cb* pCb; const void* pDyn;
  u64 layout, renderPass; u32 subpass; u64 basePipe; i32 baseIdx;
} vk_gfx_pipe_ci;

typedef struct { u32 sType; const void* pNext; u32 flags; u32 qfIdx; } vk_cp_ci;
typedef struct { u32 sType; const void* pNext; u64 pool; u32 level, cnt; } vk_cb_ai;
typedef struct { u32 sType; const void* pNext; u32 flags; const void* pInh; } vk_cb_bi;
typedef struct {
  u32 sType; const void* pNext; u64 renderPass, framebuffer; vk_rect2d area;
  u32 clearCount; const float (*pClear)[4];
} vk_rp_bi;
typedef struct {
  u32 sType; const void* pNext; u32 waitCnt; const u64* pWaits; const u32* pStages;
  u32 cbCnt; void* const* pCbs; u32 sigCnt; const u64* pSigs;
} vk_submit_info;

__attribute__((dllimport)) i32 VAPI vkEnumerateInstanceVersion(u32* pVer);
__attribute__((dllimport)) i32 VAPI vkCreateInstance(const vk_inst_ci*, const void*, void**);
__attribute__((dllimport)) void VAPI vkDestroyInstance(void*, const void*);
__attribute__((dllimport)) i32 VAPI vkEnumeratePhysicalDevices(void*, u32*, void**);
__attribute__((dllimport)) void VAPI vkGetPhysicalDeviceProperties(void*, vk_pd_props*);
__attribute__((dllimport)) void VAPI vkGetPhysicalDeviceQueueFamilyProperties(void*, u32*, vk_qf_props*);
__attribute__((dllimport)) void VAPI vkGetPhysicalDeviceMemoryProperties(void*, vk_mem_props*);
__attribute__((dllimport)) i32 VAPI vkCreateWin32SurfaceKHR(void*, const vk_win32_surf_ci*, const void*, u64*);
__attribute__((dllimport)) void VAPI vkDestroySurfaceKHR(void*, u64, const void*);
__attribute__((dllimport)) i32 VAPI vkCreateDevice(void*, const vk_dev_ci*, const void*, void**);
__attribute__((dllimport)) void VAPI vkDestroyDevice(void*, const void*);
__attribute__((dllimport)) void VAPI vkGetDeviceQueue(void*, u32, u32, void**);
__attribute__((dllimport)) i32 VAPI vkCreateSwapchainKHR(void*, const vk_sc_ci*, const void*, u64*);
__attribute__((dllimport)) void VAPI vkDestroySwapchainKHR(void*, u64, const void*);
__attribute__((dllimport)) i32 VAPI vkGetSwapchainImagesKHR(void*, u64, u32*, u64*);
__attribute__((dllimport)) i32 VAPI vkAcquireNextImageKHR(void*, u64, u64, u64, u64, u32*);
__attribute__((dllimport)) i32 VAPI vkQueuePresentKHR(void*, const vk_present_info*);
__attribute__((dllimport)) i32 VAPI vkCreateBuffer(void*, const vk_buf_ci*, const void*, u64*);
__attribute__((dllimport)) void VAPI vkDestroyBuffer(void*, u64, const void*);
__attribute__((dllimport)) void VAPI vkGetBufferMemoryRequirements(void*, u64, vk_mem_reqs*);
__attribute__((dllimport)) i32 VAPI vkAllocateMemory(void*, const vk_alloc_info*, const void*, u64*);
__attribute__((dllimport)) void VAPI vkFreeMemory(void*, u64, const void*);
__attribute__((dllimport)) i32 VAPI vkMapMemory(void*, u64, u64, u64, u32, void**);
__attribute__((dllimport)) void VAPI vkUnmapMemory(void*, u64);
__attribute__((dllimport)) i32 VAPI vkBindBufferMemory(void*, u64, u64, u64);
__attribute__((dllimport)) i32 VAPI vkCreateImageView(void*, const vk_iv_ci*, const void*, u64*);
__attribute__((dllimport)) void VAPI vkDestroyImageView(void*, u64, const void*);
__attribute__((dllimport)) i32 VAPI vkCreateShaderModule(void*, const vk_sm_ci*, const void*, u64*);
__attribute__((dllimport)) void VAPI vkDestroyShaderModule(void*, u64, const void*);
__attribute__((dllimport)) i32 VAPI vkCreateDescriptorSetLayout(void*, const vk_dsl_ci*, const void*, u64*);
__attribute__((dllimport)) void VAPI vkDestroyDescriptorSetLayout(void*, u64, const void*);
__attribute__((dllimport)) i32 VAPI vkCreatePipelineLayout(void*, const vk_pl_ci*, const void*, u64*);
__attribute__((dllimport)) void VAPI vkDestroyPipelineLayout(void*, u64, const void*);
__attribute__((dllimport)) i32 VAPI vkCreateDescriptorPool(void*, const vk_dp_ci*, const void*, u64*);
__attribute__((dllimport)) void VAPI vkDestroyDescriptorPool(void*, u64, const void*);
__attribute__((dllimport)) i32 VAPI vkAllocateDescriptorSets(void*, const vk_ds_ai*, u64*);
__attribute__((dllimport)) void VAPI vkUpdateDescriptorSets(void*, u32, const vk_write_ds*, u32, const void*);
__attribute__((dllimport)) i32 VAPI vkCreateRenderPass(void*, const vk_rp_ci*, const void*, u64*);
__attribute__((dllimport)) void VAPI vkDestroyRenderPass(void*, u64, const void*);
__attribute__((dllimport)) i32 VAPI vkCreateFramebuffer(void*, const vk_fb_ci*, const void*, u64*);
__attribute__((dllimport)) void VAPI vkDestroyFramebuffer(void*, u64, const void*);
__attribute__((dllimport)) i32 VAPI vkCreateGraphicsPipelines(void*, u64, u32, const vk_gfx_pipe_ci*, const void*, u64*);
__attribute__((dllimport)) void VAPI vkDestroyPipeline(void*, u64, const void*);
__attribute__((dllimport)) i32 VAPI vkCreateCommandPool(void*, const vk_cp_ci*, const void*, u64*);
__attribute__((dllimport)) void VAPI vkDestroyCommandPool(void*, u64, const void*);
__attribute__((dllimport)) i32 VAPI vkAllocateCommandBuffers(void*, const vk_cb_ai*, void**);
__attribute__((dllimport)) i32 VAPI vkBeginCommandBuffer(void*, const vk_cb_bi*);
__attribute__((dllimport)) i32 VAPI vkEndCommandBuffer(void*);
__attribute__((dllimport)) void VAPI vkCmdBeginRenderPass(void*, const vk_rp_bi*, u32);
__attribute__((dllimport)) void VAPI vkCmdEndRenderPass(void*);
__attribute__((dllimport)) void VAPI vkCmdBindPipeline(void*, u32, u64);
__attribute__((dllimport)) void VAPI vkCmdBindVertexBuffers(void*, u32, u32, const u64*, const u64*);
__attribute__((dllimport)) void VAPI vkCmdBindDescriptorSets(void*, u32, u64, u32, u32, const u64*, u32, const u32*);
__attribute__((dllimport)) void VAPI vkCmdPushConstants(void*, u64, u32, u32, u32, const void*);
__attribute__((dllimport)) void VAPI vkCmdDraw(void*, u32, u32, u32, u32);
__attribute__((dllimport)) i32 VAPI vkQueueSubmit(void*, u32, const vk_submit_info*, u64);
__attribute__((dllimport)) i32 VAPI vkDeviceWaitIdle(void*);
__attribute__((dllimport)) void* VAPI vkGetInstanceProcAddr(void*, const char*);

#define SPV(op, wc) (((u32)(wc) << 16) | (u32)(op))

u32 guest_vulkan_main(void) {
  u32 mask = 0;

  u32 ver = 0;
  if (vkEnumerateInstanceVersion(&ver) == 0 && ver >= 0x00401000u) mask |= 0x0001u;

  vk_inst_ci ici; zclear(&ici, sizeof(ici)); ici.sType = 1u;
  void* inst = 0;
  if (vkCreateInstance(&ici, 0, &inst) != 0 || !inst) return mask;
  mask |= 0x0002u;

  void* gpu = 0;
  u32 gpu_cnt = 1;
  if (vkEnumeratePhysicalDevices(inst, &gpu_cnt, &gpu) == 0 && gpu) {
    vk_pd_props pdp; zclear(&pdp, sizeof(pdp));
    vkGetPhysicalDeviceProperties(gpu, &pdp);
    u32 qcnt = 1; vk_qf_props qfp; zclear(&qfp, sizeof(qfp));
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qcnt, &qfp);
    vk_mem_props mp; zclear(&mp, sizeof(mp));
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    if (pdp.apiVersion && qcnt >= 1 && mp.memTypeCount >= 1) mask |= 0x0004u;
  }

  vk_win32_surf_ci sci; zclear(&sci, sizeof(sci));
  sci.sType = 1000009000u; sci.hwnd = (void*)0x100ull;
  u64 surf = 0;
  if (vkCreateWin32SurfaceKHR(inst, &sci, 0, &surf) == 0 && surf) mask |= 0x0008u;

  float prio = 1.0f;
  vk_dq_ci dqci; zclear(&dqci, sizeof(dqci));
  dqci.sType = 2u; dqci.queueCount = 1; dqci.pQueuePriorities = &prio;
  vk_dev_ci dci; zclear(&dci, sizeof(dci));
  dci.sType = 3u; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &dqci;
  void* dev = 0;
  if (vkCreateDevice(gpu, &dci, 0, &dev) != 0 || !dev) {
    vkDestroyInstance(inst, 0);
    return mask;
  }
  void* queue = 0;
  vkGetDeviceQueue(dev, 0, 0, &queue);
  if (queue) mask |= 0x0010u;

  vk_sc_ci scci; zclear(&scci, sizeof(scci));
  scci.sType = 1000001000u; scci.surface = surf; scci.minImageCount = 2;
  scci.imageFormat = 44u; scci.imageExtent.width = 1280; scci.imageExtent.height = 720;
  scci.imageArrayLayers = 1; scci.imageUsage = 0x10u;
  u64 sc = 0; u64 sc_imgs[4] = {0, 0, 0, 0}; u32 sc_cnt = 4; u32 img_idx = 99;
  if (vkCreateSwapchainKHR(dev, &scci, 0, &sc) == 0 && sc &&
      vkGetSwapchainImagesKHR(dev, sc, &sc_cnt, sc_imgs) == 0 && sc_cnt >= 2 &&
      vkAcquireNextImageKHR(dev, sc, 0xFFFFFFFFull, 0, 0, &img_idx) == 0 &&
      img_idx < sc_cnt) {
    mask |= 0x0020u;
  }

  // Vertex buffer + Uniform buffer backed by mapped VkDeviceMemory
  float tri_vb[12] = {0.0f, -0.5f, 0.0f, 1.0f,  0.5f, 0.5f, 0.0f, 1.0f,  -0.5f, 0.5f, 0.0f, 1.0f};
  vk_buf_ci vb_ci; zclear(&vb_ci, sizeof(vb_ci));
  vb_ci.sType = 12u; vb_ci.size = sizeof(tri_vb); vb_ci.usage = 0x80u;
  u64 vb = 0, vb_mem = 0;
  vkCreateBuffer(dev, &vb_ci, 0, &vb);
  vk_mem_reqs mr; zclear(&mr, sizeof(mr));
  vkGetBufferMemoryRequirements(dev, vb, &mr);
  vk_alloc_info ai; zclear(&ai, sizeof(ai)); ai.sType = 5u; ai.allocationSize = mr.size;
  vkAllocateMemory(dev, &ai, 0, &vb_mem);
  void* vb_map = 0;
  if (vkMapMemory(dev, vb_mem, 0, sizeof(tri_vb), 0, &vb_map) == 0 && vb_map) {
    float* dst = (float*)vb_map;
    for (u32 i = 0; i < 12; ++i) dst[i] = tri_vb[i];
    vkUnmapMemory(dev, vb_mem);
  }
  if (vkBindBufferMemory(dev, vb, vb_mem, 0) == 0) mask |= 0x0040u;

  float ubo_col[4] = {0.1f, 0.9f, 0.4f, 1.0f};
  vk_buf_ci ubo_ci; zclear(&ubo_ci, sizeof(ubo_ci));
  ubo_ci.sType = 12u; ubo_ci.size = sizeof(ubo_col); ubo_ci.usage = 0x10u;
  u64 ubo = 0, ubo_mem = 0;
  vkCreateBuffer(dev, &ubo_ci, 0, &ubo);
  ai.allocationSize = 64u;
  vkAllocateMemory(dev, &ai, 0, &ubo_mem);
  void* ubo_map = 0;
  if (vkMapMemory(dev, ubo_mem, 0, sizeof(ubo_col), 0, &ubo_map) == 0 && ubo_map) {
    float* dst = (float*)ubo_map;
    for (u32 i = 0; i < 4; ++i) dst[i] = ubo_col[i];
    vkUnmapMemory(dev, ubo_mem);
  }
  vkBindBufferMemory(dev, ubo, ubo_mem, 0);

  // Minimal valid SPIR-V VS (vec4 inPos@loc0 -> gl_Position) & PS (UBO vec4 -> outColor@loc0)
  static const u32 k_vs_spv[] = {
      0x07230203u, 0x00010000u, 0u, 16u, 0u,
      SPV(17, 2), 1u,                    // OpCapability Shader
      SPV(14, 3), 0u, 1u,                // OpMemoryModel Logical GLSL450
      SPV(15, 7), 0u, 1u, 0x6E69616Du, 0u, 2u, 3u, // OpEntryPoint Vertex %1 "main" %2 %3
      SPV(71, 4), 2u, 30u, 0u,           // OpDecorate %2 Location 0
      SPV(71, 4), 3u, 11u, 0u,           // OpDecorate %3 BuiltIn Position
      SPV(19, 2), 4u,                    // OpTypeVoid %4
      SPV(33, 3), 5u, 4u,                // OpTypeFunction %5 %4
      SPV(22, 3), 6u, 32u,               // OpTypeFloat %6 32
      SPV(23, 4), 7u, 6u, 4u,            // OpTypeVector %7 %6 4
      SPV(32, 4), 8u, 1u, 7u,            // OpTypePointer Input %7
      SPV(32, 4), 9u, 3u, 7u,            // OpTypePointer Output %7
      SPV(59, 4), 8u, 2u, 1u,            // OpVariable %inPos Input
      SPV(59, 4), 9u, 3u, 3u,            // OpVariable %outPos Output
      SPV(54, 5), 4u, 1u, 0u, 5u,        // OpFunction %1
      SPV(248, 2), 10u,                  // OpLabel %10
      SPV(61, 4), 7u, 11u, 2u,           // OpLoad %11 = *%2
      SPV(62, 3), 3u, 11u,               // OpStore *%3 = %11
      SPV(253, 1),                       // OpReturn
      SPV(56, 1)                         // OpFunctionEnd
  };
  static const u32 k_ps_spv[] = {
      0x07230203u, 0x00010000u, 0u, 20u, 0u,
      SPV(17, 2), 1u,
      SPV(14, 3), 0u, 1u,
      SPV(15, 6), 4u, 1u, 0x6E69616Du, 0u, 2u, // OpEntryPoint Fragment %1 "main" %2
      SPV(16, 3), 1u, 7u,                // OpExecutionMode OriginUpperLeft
      SPV(71, 4), 2u, 30u, 0u,           // OpDecorate %outCol Location 0
      SPV(71, 3), 10u, 2u,               // OpDecorate %uboStruct Block
      SPV(72, 5), 10u, 0u, 35u, 0u,      // OpMemberDecorate %uboStruct 0 Offset 0
      SPV(71, 4), 12u, 34u, 0u,          // OpDecorate %uboVar DescriptorSet 0
      SPV(71, 4), 12u, 33u, 0u,          // OpDecorate %uboVar Binding 0
      SPV(19, 2), 4u,
      SPV(33, 3), 5u, 4u,
      SPV(22, 3), 6u, 32u,
      SPV(23, 4), 7u, 6u, 4u,            // vec4
      SPV(21, 4), 8u, 32u, 1u,           // int32
      SPV(43, 4), 8u, 9u, 0u,            // const int 0
      SPV(30, 3), 10u, 7u,               // OpTypeStruct %7
      SPV(32, 4), 11u, 2u, 10u,          // ptr Uniform struct
      SPV(32, 4), 13u, 2u, 7u,           // ptr Uniform vec4
      SPV(32, 4), 14u, 3u, 7u,           // ptr Output vec4
      SPV(59, 4), 11u, 12u, 2u,          // %uboVar
      SPV(59, 4), 14u, 2u, 3u,           // %outCol
      SPV(54, 5), 4u, 1u, 0u, 5u,
      SPV(248, 2), 15u,
      SPV(65, 5), 13u, 16u, 12u, 9u,     // OpAccessChain
      SPV(61, 4), 7u, 17u, 16u,          // OpLoad
      SPV(62, 3), 2u, 17u,               // OpStore
      SPV(253, 1),
      SPV(56, 1)
  };

  vk_sm_ci vsm_ci = {15u, 0, 0, sizeof(k_vs_spv), k_vs_spv};
  vk_sm_ci psm_ci = {15u, 0, 0, sizeof(k_ps_spv), k_ps_spv};
  u64 vs_mod = 0, ps_mod = 0;
  if (vkCreateShaderModule(dev, &vsm_ci, 0, &vs_mod) == 0 &&
      vkCreateShaderModule(dev, &psm_ci, 0, &ps_mod) == 0) {
    mask |= 0x0080u;
  }

  vk_dsl_bind dslb = {0u, 6u /*UNIFORM_BUFFER*/, 1u, 0x10u, 0};
  vk_dsl_ci dsl_ci = {32u, 0, 0, 1u, &dslb};
  u64 dsl = 0;
  vkCreateDescriptorSetLayout(dev, &dsl_ci, 0, &dsl);

  vk_pc_range pcr = {0x11u, 0u, 16u};
  vk_pl_ci pl_ci = {30u, 0, 0, 1u, &dsl, 1u, &pcr};
  u64 pipe_layout = 0;
  vkCreatePipelineLayout(dev, &pl_ci, 0, &pipe_layout);

  vk_dp_size dps = {6u, 4u};
  vk_dp_ci dp_ci = {33u, 0, 0, 4u, 1u, &dps};
  u64 dpool = 0, dset = 0;
  vkCreateDescriptorPool(dev, &dp_ci, 0, &dpool);
  vk_ds_ai ds_ai = {34u, 0, dpool, 1u, &dsl};
  if (vkAllocateDescriptorSets(dev, &ds_ai, &dset) == 0 && dset) {
    vk_db_info dbi = {ubo, 0, 16u};
    vk_write_ds wds; zclear(&wds, sizeof(wds));
    wds.sType = 35u; wds.dstSet = dset; wds.dstBinding = 0;
    wds.descriptorCount = 1; wds.descriptorType = 6u; wds.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(dev, 1, &wds, 0, 0);
    mask |= 0x0100u;
  }

  vk_att_desc att; zclear(&att, sizeof(att));
  att.format = 44u; att.samples = 1; att.loadOp = 1;
  vk_att_ref cref = {0u, 2u};
  vk_subpass sp; zclear(&sp, sizeof(sp));
  sp.colCnt = 1; sp.pCol = &cref;
  vk_rp_ci rp_ci; zclear(&rp_ci, sizeof(rp_ci));
  rp_ci.sType = 38u; rp_ci.attachmentCount = 1; rp_ci.pAttachments = &att;
  rp_ci.subpassCount = 1; rp_ci.pSubpasses = &sp;
  u64 rp = 0;
  vkCreateRenderPass(dev, &rp_ci, 0, &rp);

  vk_iv_ci iv_ci; zclear(&iv_ci, sizeof(iv_ci));
  iv_ci.sType = 15u; iv_ci.image = sc_imgs[0]; iv_ci.viewType = 1u; iv_ci.format = 44u;
  u64 sc_view = 0;
  vkCreateImageView(dev, &iv_ci, 0, &sc_view);
  vk_fb_ci fb_ci = {37u, 0, 0, rp, 1u, &sc_view, 1280u, 720u, 1u};
  u64 fb = 0;
  if (vkCreateFramebuffer(dev, &fb_ci, 0, &fb) == 0 && fb) mask |= 0x0200u;

  vk_pipe_stage stgs[2]; zclear(stgs, sizeof(stgs));
  stgs[0].sType = 18u; stgs[0].stage = 0x01u; stgs[0].module = vs_mod; stgs[0].pName = "main";
  stgs[1].sType = 18u; stgs[1].stage = 0x10u; stgs[1].module = ps_mod; stgs[1].pName = "main";
  vk_vb_desc vbd = {0u, 16u, 0u};
  vk_va_desc vad = {0u, 0u, 109u /*R32G32B32A32_SFLOAT*/, 0u};
  vk_pipe_vi vi = {21u, 0, 0, 1u, &vbd, 1u, &vad};
  vk_pipe_ia ia = {22u, 0, 0, 3u, 0u};
  vk_vp vp = {0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f};
  vk_rect2d sc_rect = {{0, 0}, {1280u, 720u}};
  vk_pipe_vp vps = {24u, 0, 0, 1u, &vp, 1u, &sc_rect};
  vk_pipe_rs rs; zclear(&rs, sizeof(rs)); rs.sType = 25u; rs.lineW = 1.0f;
  vk_cb_att cba; zclear(&cba, sizeof(cba)); cba.mask = 0xFu;
  vk_pipe_cb cb; zclear(&cb, sizeof(cb)); cb.sType = 26u; cb.attCnt = 1; cb.pAtts = &cba;
  vk_gfx_pipe_ci gp_ci; zclear(&gp_ci, sizeof(gp_ci));
  gp_ci.sType = 28u; gp_ci.stageCount = 2; gp_ci.pStages = stgs;
  gp_ci.pVi = &vi; gp_ci.pIa = &ia; gp_ci.pVp = &vps; gp_ci.pRs = &rs; gp_ci.pCb = &cb;
  gp_ci.layout = pipe_layout; gp_ci.renderPass = rp;
  u64 pipe = 0;
  if (vkCreateGraphicsPipelines(dev, 0, 1u, &gp_ci, 0, &pipe) == 0 && pipe) mask |= 0x0400u;

  vk_cp_ci cp_ci = {39u, 0, 0, 0};
  u64 cpool = 0; void* cmd = 0;
  vkCreateCommandPool(dev, &cp_ci, 0, &cpool);
  vk_cb_ai cb_ai = {40u, 0, cpool, 0u, 1u};
  if (vkAllocateCommandBuffers(dev, &cb_ai, &cmd) == 0 && cmd) mask |= 0x0800u;

  vk_cb_bi cbbi = {42u, 0, 0, 0};
  if (vkBeginCommandBuffer(cmd, &cbbi) == 0) {
    float clr[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    vk_rp_bi rbi = {43u, 0, rp, fb, {{0, 0}, {1280u, 720u}}, 1u, &clr};
    vkCmdBeginRenderPass(cmd, &rbi, 0u);
    vkCmdBindPipeline(cmd, 0u, pipe);
    u64 vb_off = 0;
    vkCmdBindVertexBuffers(cmd, 0u, 1u, &vb, &vb_off);
    vkCmdBindDescriptorSets(cmd, 0u, pipe_layout, 0u, 1u, &dset, 0u, 0);
    float push_v[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    vkCmdPushConstants(cmd, pipe_layout, 0x11u, 0u, 16u, push_v);
    vkCmdDraw(cmd, 3u, 1u, 0u, 0u);
    vkCmdEndRenderPass(cmd);
    if (vkEndCommandBuffer(cmd) == 0) mask |= 0x1000u;
  }

  vk_submit_info si; zclear(&si, sizeof(si));
  si.sType = 4u; si.cbCnt = 1; si.pCbs = &cmd;
  if (vkQueueSubmit(queue, 1u, &si, 0) == 0) mask |= 0x2000u;

  i32 pres_rc = -1;
  vk_present_info pi; zclear(&pi, sizeof(pi));
  pi.sType = 1000001001u; pi.swapchainCount = 1; pi.pSwapchains = &sc;
  pi.pImageIndices = &img_idx; pi.pResults = &pres_rc;
  if (vkQueuePresentKHR(queue, &pi) == 0 && pres_rc == 0) mask |= 0x4000u;

  if (vkGetInstanceProcAddr(inst, "vkCmdBeginRendering") != 0 &&
      vkDeviceWaitIdle(dev) == 0) {
    mask |= 0x8000u;
  }

  vkDestroyCommandPool(dev, cpool, 0);
  vkDestroyPipeline(dev, pipe, 0);
  vkDestroyFramebuffer(dev, fb, 0);
  vkDestroyImageView(dev, sc_view, 0);
  vkDestroyRenderPass(dev, rp, 0);
  vkDestroyDescriptorPool(dev, dpool, 0);
  vkDestroyPipelineLayout(dev, pipe_layout, 0);
  vkDestroyDescriptorSetLayout(dev, dsl, 0);
  vkDestroyShaderModule(dev, ps_mod, 0);
  vkDestroyShaderModule(dev, vs_mod, 0);
  vkDestroyBuffer(dev, ubo, 0);
  vkFreeMemory(dev, ubo_mem, 0);
  vkDestroyBuffer(dev, vb, 0);
  vkFreeMemory(dev, vb_mem, 0);
  vkDestroySwapchainKHR(dev, sc, 0);
  vkDestroyDevice(dev, 0);
  vkDestroySurfaceKHR(inst, surf, 0);
  vkDestroyInstance(inst, 0);
  return mask;
}
