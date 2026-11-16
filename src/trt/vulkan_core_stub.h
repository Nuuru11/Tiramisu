// Minimal Vulkan type definitions for the UWP cross-build, where the real
// <vulkan/vulkan_core.h> is unavailable in the xwin SDK. Only the types the
// Vulkan->D3D11 bridge actually references are defined here, with the same
// sizes/offsets the Linux dev host verifies via static_assert. On Linux the
// real header is used (see the __has_include guard in bridge_vulkan.cpp).
#ifndef TIRAMISU_VULKAN_CORE_STUB_H
#define TIRAMISU_VULKAN_CORE_STUB_H

#include <cstdint>

#define VK_TRUE 1
#define VK_FALSE 0
#define VK_MAX_PHYSICAL_DEVICE_NAME_SIZE 256
#define VK_UUID_SIZE 16

typedef uint32_t VkBool32;
typedef uint32_t VkStructureType;
typedef uint32_t VkSampleCountFlags;
typedef uint64_t VkDeviceSize;
typedef enum VkPhysicalDeviceType {
    VK_PHYSICAL_DEVICE_TYPE_OTHER = 0,
    VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
    VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2,
    VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU = 3,
    VK_PHYSICAL_DEVICE_TYPE_CPU = 4,
} VkPhysicalDeviceType;

typedef struct VkPhysicalDeviceSparseProperties {
    VkBool32 residencyStandard2DBlockShape;
    VkBool32 residencyStandard2DMultisampleBlockShape;
    VkBool32 residencyStandard3DBlockShape;
    VkBool32 residencyAlignedMipSize;
    VkBool32 residencyNonResidentStrict;
} VkPhysicalDeviceSparseProperties;

typedef struct VkPhysicalDeviceLimits {
    uint32_t              maxImageDimension1D;
        uint32_t              maxImageDimension2D;
        uint32_t              maxImageDimension3D;
        uint32_t              maxImageDimensionCube;
        uint32_t              maxImageArrayLayers;
        uint32_t              maxTexelBufferElements;
        uint32_t              maxUniformBufferRange;
        uint32_t              maxStorageBufferRange;
        uint32_t              maxPushConstantsSize;
        uint32_t              maxMemoryAllocationCount;
        uint32_t              maxSamplerAllocationCount;
        VkDeviceSize          bufferImageGranularity;
        VkDeviceSize          sparseAddressSpaceSize;
        uint32_t              maxBoundDescriptorSets;
        uint32_t              maxPerStageDescriptorSamplers;
        uint32_t              maxPerStageDescriptorUniformBuffers;
        uint32_t              maxPerStageDescriptorStorageBuffers;
        uint32_t              maxPerStageDescriptorSampledImages;
        uint32_t              maxPerStageDescriptorStorageImages;
        uint32_t              maxPerStageDescriptorInputAttachments;
        uint32_t              maxPerStageResources;
        uint32_t              maxDescriptorSetSamplers;
        uint32_t              maxDescriptorSetUniformBuffers;
        uint32_t              maxDescriptorSetUniformBuffersDynamic;
        uint32_t              maxDescriptorSetStorageBuffers;
        uint32_t              maxDescriptorSetStorageBuffersDynamic;
        uint32_t              maxDescriptorSetSampledImages;
        uint32_t              maxDescriptorSetStorageImages;
        uint32_t              maxDescriptorSetInputAttachments;
        uint32_t              maxVertexInputAttributes;
        uint32_t              maxVertexInputBindings;
        uint32_t              maxVertexInputAttributeOffset;
        uint32_t              maxVertexInputBindingStride;
        uint32_t              maxVertexOutputComponents;
        uint32_t              maxTessellationGenerationLevel;
        uint32_t              maxTessellationPatchSize;
        uint32_t              maxTessellationControlPerVertexInputComponents;
        uint32_t              maxTessellationControlPerVertexOutputComponents;
        uint32_t              maxTessellationControlPerPatchOutputComponents;
        uint32_t              maxTessellationControlTotalOutputComponents;
        uint32_t              maxTessellationEvaluationInputComponents;
        uint32_t              maxTessellationEvaluationOutputComponents;
        uint32_t              maxGeometryShaderInvocations;
        uint32_t              maxGeometryInputComponents;
        uint32_t              maxGeometryOutputComponents;
        uint32_t              maxGeometryOutputVertices;
        uint32_t              maxGeometryTotalOutputComponents;
        uint32_t              maxFragmentInputComponents;
        uint32_t              maxFragmentOutputAttachments;
        uint32_t              maxFragmentDualSrcAttachments;
        uint32_t              maxFragmentCombinedOutputResources;
        uint32_t              maxComputeSharedMemorySize;
        uint32_t              maxComputeWorkGroupCount[3];
        uint32_t              maxComputeWorkGroupInvocations;
        uint32_t              maxComputeWorkGroupSize[3];
        uint32_t              subPixelPrecisionBits;
        uint32_t              subTexelPrecisionBits;
        uint32_t              mipmapPrecisionBits;
        uint32_t              maxDrawIndexedIndexValue;
        uint32_t              maxDrawIndirectCount;
        float                 maxSamplerLodBias;
        float                 maxSamplerAnisotropy;
        uint32_t              maxViewports;
        uint32_t              maxViewportDimensions[2];
        float                 viewportBoundsRange[2];
        uint32_t              viewportSubPixelBits;
        size_t                minMemoryMapAlignment;
        VkDeviceSize          minTexelBufferOffsetAlignment;
        VkDeviceSize          minUniformBufferOffsetAlignment;
        VkDeviceSize          minStorageBufferOffsetAlignment;
        int32_t               minTexelOffset;
        uint32_t              maxTexelOffset;
        int32_t               minTexelGatherOffset;
        uint32_t              maxTexelGatherOffset;
        float                 minInterpolationOffset;
        float                 maxInterpolationOffset;
        uint32_t              subPixelInterpolationOffsetBits;
        uint32_t              maxFramebufferWidth;
        uint32_t              maxFramebufferHeight;
        uint32_t              maxFramebufferLayers;
        VkSampleCountFlags    framebufferColorSampleCounts;
        VkSampleCountFlags    framebufferDepthSampleCounts;
        VkSampleCountFlags    framebufferStencilSampleCounts;
        VkSampleCountFlags    framebufferNoAttachmentsSampleCounts;
        uint32_t              maxColorAttachments;
        VkSampleCountFlags    sampledImageColorSampleCounts;
        VkSampleCountFlags    sampledImageIntegerSampleCounts;
        VkSampleCountFlags    sampledImageDepthSampleCounts;
        VkSampleCountFlags    sampledImageStencilSampleCounts;
        VkSampleCountFlags    storageImageSampleCounts;
        uint32_t              maxSampleMaskWords;
        VkBool32              timestampComputeAndGraphics;
        float                 timestampPeriod;
        uint32_t              maxClipDistances;
        uint32_t              maxCullDistances;
        uint32_t              maxCombinedClipAndCullDistances;
        uint32_t              discreteQueuePriorities;
        float                 pointSizeRange[2];
        float                 lineWidthRange[2];
        float                 pointSizeGranularity;
        float                 lineWidthGranularity;
        VkBool32              strictLines;
        VkBool32              standardSampleLocations;
        VkDeviceSize          optimalBufferCopyOffsetAlignment;
        VkDeviceSize          optimalBufferCopyRowPitchAlignment;
        VkDeviceSize          nonCoherentAtomSize;
} VkPhysicalDeviceLimits;

typedef struct VkPhysicalDeviceProperties {
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    VkPhysicalDeviceType deviceType;
    char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint8_t pipelineCacheUUID[VK_UUID_SIZE];
    VkPhysicalDeviceLimits limits;
    VkPhysicalDeviceSparseProperties sparseProperties;
} VkPhysicalDeviceProperties;

typedef struct VkPhysicalDeviceFeatures {
    VkBool32 robustBufferAccess;
    VkBool32 fullDrawIndexUint32;
    VkBool32 imageCubeArray;
    VkBool32 independentBlend;
    VkBool32 geometryShader;
    VkBool32 tessellationShader;
    VkBool32 sampleRateShading;
    VkBool32 dualSrcBlend;
    VkBool32 logicOp;
    VkBool32 multiDrawIndirect;
    VkBool32 drawIndirectFirstInstance;
    VkBool32 depthClamp;
    VkBool32 depthBiasClamp;
    VkBool32 fillModeNonSolid;
    VkBool32 depthBounds;
    VkBool32 wideLines;
    VkBool32 largePoints;
    VkBool32 alphaToOne;
    VkBool32 multiViewport;
    VkBool32 samplerAnisotropy;
    VkBool32 textureCompressionETC2;
    VkBool32 textureCompressionASTC_LDR;
    VkBool32 textureCompressionBC;
    VkBool32 occlusionQueryPrecise;
    VkBool32 pipelineStatisticsQuery;
    VkBool32 vertexPipelineStoresAndAtomics;
    VkBool32 fragmentStoresAndAtomics;
    VkBool32 shaderTessellationAndGeometryPointSize;
    VkBool32 shaderImageGatherExtended;
    VkBool32 shaderStorageImageExtendedFormats;
    VkBool32 shaderStorageImageMultisample;
    VkBool32 shaderStorageImageReadWithoutFormat;
    VkBool32 shaderStorageImageWriteWithoutFormat;
    VkBool32 shaderUniformBufferArrayDynamicIndexing;
    VkBool32 shaderSampledImageArrayDynamicIndexing;
    VkBool32 shaderStorageBufferArrayDynamicIndexing;
    VkBool32 shaderStorageImageArrayDynamicIndexing;
    VkBool32 shaderClipDistance;
    VkBool32 shaderCullDistance;
    VkBool32 shaderFloat64;
    VkBool32 shaderInt64;
    VkBool32 shaderInt16;
    VkBool32 shaderResourceResidency;
    VkBool32 shaderResourceMinLod;
    VkBool32 sparseBinding;
    VkBool32 sparseResidencyBuffer;
    VkBool32 sparseResidencyImage2D;
    VkBool32 sparseResidencyImage3D;
    VkBool32 sparseResidency2Samples;
    VkBool32 sparseResidency4Samples;
    VkBool32 sparseResidency8Samples;
    VkBool32 sparseResidency16Samples;
    VkBool32 sparseResidencyAliased;
    VkBool32 variableMultisampleRate;
    VkBool32 inheritedQueries;
} VkPhysicalDeviceFeatures;

typedef struct VkPhysicalDeviceFeatures2 {
    VkStructureType sType;
    void* pNext;
    VkPhysicalDeviceFeatures features;
} VkPhysicalDeviceFeatures2;

typedef struct VkPhysicalDeviceProperties2 {
    VkStructureType sType;
    void* pNext;
    VkPhysicalDeviceProperties properties;
} VkPhysicalDeviceProperties2;

typedef VkDeviceSize uint64_t;

#endif // TIRAMISU_VULKAN_CORE_STUB_H
