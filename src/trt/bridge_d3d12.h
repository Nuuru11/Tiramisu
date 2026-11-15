// tiramisu/src/trt/bridge_d3d12.h
// Direct3D 12 bridge (d3d12.dll + d3d12core.dll):
//   - On Xbox UWP: forwards D3D12 entry points to the real hardware d3d12.dll
//     while integrating with DXGI CoreWindow swapchain management in bridge_d3d.
//   - Offline / Linux CI: provides a full stateful software D3D12 COM runtime
//     (ID3D12Device4, ID3D12CommandQueue, ID3D12GraphicsCommandList, ID3D12Fence,
//     ID3D12Resource, ID3D12DescriptorHeap, ID3D12RootSignature, PipelineState,
//     and DXBC root-signature serialization) matching MSVC Win64 vtable/ABI layouts.
#ifndef TIRAMISU_BRIDGE_D3D12_H
#define TIRAMISU_BRIDGE_D3D12_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct trt_d3d12_hooks {
  int (TRT_GUEST_ABI *create_device)(void* adapter, u32 min_fl, const void* riid,
                                     void** out_dev);
  int (TRT_GUEST_ABI *get_debug_interface)(const void* riid, void** out_dbg);
  int (TRT_GUEST_ABI *serialize_root_sig)(const void* desc, u32 version,
                                          void** out_blob, void** out_err);
  int (TRT_GUEST_ABI *serialize_versioned_root_sig)(const void* desc,
                                                    void** out_blob,
                                                    void** out_err);
  int (TRT_GUEST_ABI *create_root_deserializer)(const void* src, u64 size,
                                                const void* riid, void** out_des);
  int (TRT_GUEST_ABI *create_versioned_root_deserializer)(const void* src,
                                                          u64 size,
                                                          const void* riid,
                                                          void** out_des);
  int (TRT_GUEST_ABI *get_interface)(const void* rclsid, const void* riid,
                                     void** out_iface);
  int (TRT_GUEST_ABI *enable_experimental)(u32 num_features, const void* iids,
                                           void* config_structs, const u32* sizes);
} trt_d3d12_hooks;

void trt_d3d12_set_hooks(const trt_d3d12_hooks* hooks);
const trt_d3d12_hooks* trt_d3d12_hooks_get(void);

// Returns 1 if `obj` implements ID3D12CommandQueue (offline bridge object or
// live COM QueryInterface for IID_ID3D12CommandQueue).
int trt_d3d12_is_queue(void* obj);

// Returns an offline ID3D12Resource* representing swapchain backbuffer `buffer_idx`.
int trt_d3d12_offline_backbuffer(u32 buffer_idx, u32 width, u32 height,
                                 const void* riid, void** out_res);

int trt_bridge_register_d3d12(void);

int TRT_GUEST_ABI trt_D3D12CreateDevice(void* adapter, u32 min_fl,
                                      const void* riid, void** out_dev);
int TRT_GUEST_ABI trt_D3D12GetDebugInterface(const void* riid, void** out_dbg);
int TRT_GUEST_ABI trt_D3D12SerializeRootSignature(const void* desc, u32 version,
                                                  void** out_blob, void** out_err);
int TRT_GUEST_ABI trt_D3D12SerializeVersionedRootSignature(const void* desc,
                                                           void** out_blob,
                                                           void** out_err);
int TRT_GUEST_ABI trt_D3D12GetInterface(const void* rclsid, const void* riid,
                                        void** out_iface);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_BRIDGE_D3D12_H
