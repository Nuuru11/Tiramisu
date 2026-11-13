// tiramisu/src/trt/bridge_d3d.cpp
// The D3D11/DXGI bridge (see bridge_d3d.h).
//
// Only two things need shimming:
//   1. D3D11CreateDevice / D3D11CreateDeviceAndSwapChain: the guest must
//      get the HOST's device and context, otherwise its resources live in
//      a device that cannot touch the host's swapchain.
//   2. CreateDXGIFactory*: the guest's factory must hand out the HOST's
//      swapchain, because the guest's window is a fake HWND and the
//      console has no real window for a flip swapchain.
//
// The factory shim is a COM object: the guest calls it through the
// standard vtable, so the vtable layout has to match IDXGIFactory2
// exactly (IUnknown + IDXGIObject + IDXGIFactory + IDXGIFactory1 +
// IDXGIFactory2 = 25 slots).
#include "bridge_d3d.h"
#include "thunk.h"

#include <stdio.h>
#include <string.h>

namespace {

trt_gpu_hooks g_hooks;

constexpr int TRT_E_NOTIMPL = 0x80004001;  // E_NOTIMPL
constexpr int TRT_E_NOINTERFACE = 0x80004002;

// ---------------------------------------------------------------------------
// the fake factory: a COM object with the real IDXGIFactory2 vtable
// ---------------------------------------------------------------------------
struct fake_factory {
  const void** vtbl;
  u32 refcount;
};

const void** g_real_factory_vtbl = nullptr;  // the console's real factory

// forward a call to the console's real factory (same slot, same args)
void* g_real_vtbl_slot(int index) {
  if (!g_real_factory_vtbl) return nullptr;
  return (void*)g_real_factory_vtbl[index];
}

// --- IUnknown ---
int TRT_GUEST_ABI fake_QueryInterface(void* self, const void* riid, void** ppv) {
  (void)riid;
  if (!ppv) return TRT_E_NOINTERFACE;
  *ppv = self;  // we are the factory for every factory interface
  fake_factory* f = (fake_factory*)self;
  ++f->refcount;
  return 0;
}
u32 TRT_GUEST_ABI fake_AddRef(void* self) {
  fake_factory* f = (fake_factory*)self;
  return ++f->refcount;
}
u32 TRT_GUEST_ABI fake_Release(void* self) {
  fake_factory* f = (fake_factory*)self;
  return f->refcount ? --f->refcount : 0;
}

// --- IDXGIObject: nothing a game needs from us ---
int TRT_GUEST_ABI fake_SetPrivateData(void*, const void*, u32, const void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_SetPrivateDataInterface(void*, const void*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_GetPrivateData(void*, const void*, void*, u32*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_GetParent(void*, void**) { return TRT_E_NOTIMPL; }

// --- IDXGIFactory ---
int TRT_GUEST_ABI fake_EnumAdapters(void* self, u32 index, void** adapter) {
  void* slot = g_real_vtbl_slot(7);
  if (!slot || !adapter) return TRT_E_NOTIMPL;
  using fn_t = int (*)(void*, u32, void**);
  return ((fn_t)slot)(g_hooks.factory, index, adapter);
}
int TRT_GUEST_ABI fake_MakeWindowAssociation(void*, void*, u32) { return 0; }
int TRT_GUEST_ABI fake_GetWindowAssociation(void*, void**) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_CreateSwapChain(void* self, void* device, void* desc, void* hwnd,
                         void* restrict_info, void** swapchain) {
  (void)self; (void)device; (void)desc; (void)hwnd; (void)restrict_info;
  if (!swapchain) return TRT_E_NOINTERFACE;
  if (!g_hooks.swapchain) return TRT_E_NOTIMPL;
  *swapchain = g_hooks.swapchain;
  return 0;
}
int TRT_GUEST_ABI fake_CreateSoftwareAdapter(void*, void*, void**) { return TRT_E_NOTIMPL; }

// --- IDXGIFactory1 ---
int TRT_GUEST_ABI fake_EnumAdapters1(void* self, u32 index, void** adapter) {
  (void)self;
  void* slot = g_real_vtbl_slot(12);
  char line[192];
  snprintf(line, sizeof(line),
           "trt: DXGI EnumAdapters1(index=%u) via real factory slot=%p", index,
           slot);
  trt_core_log(line);
  if (!slot || !adapter) return TRT_E_NOTIMPL;
  using fn_t = int (TRT_GUEST_ABI *)(void*, u32, void**);
  return ((fn_t)slot)(g_hooks.factory, index, adapter);
}
int TRT_GUEST_ABI fake_IsCurrent(void*) { return 0; }

// --- IDXGIFactory2 ---
int TRT_GUEST_ABI fake_IsWindowedStereoEnabled(void*) { return 0; }
// IDXGIFactory2::CreateSwapChainForHwnd takes SIX arguments (the 2.x
// signature has a Flags parameter), so the out pointer is the 6th - it
// lives on the stack, not in a register. Getting this wrong hands the
// guest a garbage pointer.
int TRT_GUEST_ABI fake_CreateSwapChainForHwnd(void* self, void* device, void* desc,
                                void* hwnd, void* restrict_info, u32 flags,
                                void** swapchain) {
  (void)self; (void)device; (void)desc; (void)hwnd; (void)restrict_info;
  (void)flags;
  char line[192];
  snprintf(line, sizeof(line),
           "trt: CreateSwapChainForHwnd -> out=%p host_swapchain=%p",
           (void*)swapchain, g_hooks.swapchain);
  trt_core_log(line);
  if (!swapchain) return TRT_E_NOINTERFACE;
  if (!g_hooks.swapchain) return TRT_E_NOTIMPL;
  *swapchain = g_hooks.swapchain;
  return 0;
}
int TRT_GUEST_ABI fake_CreateSwapChainForCoreWindow(void* self, void* device, void* desc,
                                       void* core_window, void* restrict_info,
                                       void** swapchain) {
  (void)self; (void)device; (void)desc; (void)core_window; (void)restrict_info;
  if (!swapchain) return TRT_E_NOINTERFACE;
  if (!g_hooks.swapchain) return TRT_E_NOTIMPL;
  *swapchain = g_hooks.swapchain;
  return 0;
}
int TRT_GUEST_ABI fake_GetSharedResourceAdapterLuid(void*, void*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_RegisterStereoStatusWindow(void*, u32, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_RegisterStereoStatusEvent(void*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_UnregisterStereoStatus(void*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_RegisterOcclusionStatusWindow(void*, u32, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_RegisterOcclusionStatusEvent(void*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_UnregisterOcclusionStatus(void*, void*) { return TRT_E_NOTIMPL; }
int TRT_GUEST_ABI fake_CreateSwapChainForComposition(void* self, void* device, void* desc,
                                        void* restrict_info, void** swapchain) {
  (void)self; (void)device; (void)desc; (void)restrict_info;
  if (!swapchain) return TRT_E_NOINTERFACE;
  if (!g_hooks.swapchain) return TRT_E_NOTIMPL;
  *swapchain = g_hooks.swapchain;
  return 0;
}

// IDXGIFactory2 vtable: IUnknown(3) + IDXGIObject(4) + IDXGIFactory(5) +
// IDXGIFactory1(2) + IDXGIFactory2(11) = 25 slots
const void* g_factory_vtbl[25] = {
    (const void*)&fake_QueryInterface,          // 0
    (const void*)&fake_AddRef,                  // 1
    (const void*)&fake_Release,                 // 2
    (const void*)&fake_SetPrivateData,          // 3
    (const void*)&fake_SetPrivateDataInterface, // 4
    (const void*)&fake_GetPrivateData,          // 5
    (const void*)&fake_GetParent,               // 6
    (const void*)&fake_EnumAdapters,            // 7
    (const void*)&fake_MakeWindowAssociation,   // 8
    (const void*)&fake_GetWindowAssociation,    // 9
    (const void*)&fake_CreateSwapChain,         // 10
    (const void*)&fake_CreateSoftwareAdapter,   // 11
    (const void*)&fake_EnumAdapters1,           // 12
    (const void*)&fake_IsCurrent,               // 13
    (const void*)&fake_IsWindowedStereoEnabled, // 14
    (const void*)&fake_CreateSwapChainForHwnd,   // 15
    (const void*)&fake_CreateSwapChainForCoreWindow, // 16
    (const void*)&fake_GetSharedResourceAdapterLuid, // 17
    (const void*)&fake_RegisterStereoStatusWindow,   // 18
    (const void*)&fake_RegisterStereoStatusEvent,    // 19
    (const void*)&fake_UnregisterStereoStatus,       // 20
    (const void*)&fake_RegisterOcclusionStatusWindow, // 21
    (const void*)&fake_RegisterOcclusionStatusEvent,  // 22
    (const void*)&fake_UnregisterOcclusionStatus,     // 23
    (const void*)&fake_CreateSwapChainForComposition, // 24
};

fake_factory g_factory_obj = {g_factory_vtbl, 1};

} // namespace

extern "C" {


// D3D11CreateDevice(adapter, driver, sw, flags, levels, count, sdk, device,
//                    level, context)
extern "C" int TRT_GUEST_ABI trt_D3D11CreateDevice(void* adapter, u32 driver_type, void* software,
                          u32 flags, const void* feature_levels,
                          u32 feature_level_count, u32 sdk_version,
                          void** out_device, u32* out_level, void** out_context) {
  (void)adapter; (void)driver_type; (void)software; (void)flags;
  (void)feature_levels; (void)feature_level_count; (void)sdk_version;
  if (!g_hooks.device || !g_hooks.context) {
    char line[160];
    snprintf(line, sizeof(line),
             "trt: D3D11CreateDevice - no host device (host not ready?)");
    trt_core_log(line);
    return 0x887A0004;  // DXGI_ERROR_UNSUPPORTED
  }
  if (out_device) *out_device = g_hooks.device;
  if (out_context) *out_context = g_hooks.context;
  if (out_level) *out_level = g_hooks.feature_level;
  return 0;  // S_OK
}

// D3D11CreateDeviceAndSwapChain(... , out_swapchain, out_device, out_level,
//                                 out_context)
extern "C" int TRT_GUEST_ABI trt_D3D11CreateDeviceAndSwapChain(void* adapter, u32 driver_type,
                                       void* software, u32 flags,
                                       const void* feature_levels,
                                       u32 feature_level_count, u32 sdk_version,
                                       void* swapchain_desc, void** out_swapchain,
                                       void** out_device, u32* out_level,
                                       void** out_context) {
  (void)swapchain_desc;
  const int hr = trt_D3D11CreateDevice(adapter, driver_type, software, flags,
                                       feature_levels, feature_level_count,
                                       sdk_version, out_device, out_level,
                                       out_context);
  if (hr != 0) return hr;
  if (out_swapchain) *out_swapchain = g_hooks.swapchain;
  return 0;
}

// CreateDXGIFactory* return an HRESULT (S_OK) and hand the factory out
// through the out parameter - returning the pointer itself is a classic
// mistake and the guest reads it as a failing HRESULT.
extern "C" int TRT_GUEST_ABI trt_CreateDXGIFactory(void* riid, void** out_factory) {
  (void)riid;
  if (!out_factory) return TRT_E_NOINTERFACE;
  // the real factory's vtable is needed to forward adapter enumeration
  if (g_hooks.factory) {
    void** vt = (void**)g_hooks.factory;
    g_real_factory_vtbl = (const void**)vt[0];
  }
  *out_factory = (void*)&g_factory_obj;
  return 0;  // S_OK
}

extern "C" int TRT_GUEST_ABI trt_CreateDXGIFactory1(void* riid, void** out_factory) {
  return trt_CreateDXGIFactory(riid, out_factory);
}

extern "C" int TRT_GUEST_ABI trt_CreateDXGIFactory2(void* riid, void** out_factory) {
  return trt_CreateDXGIFactory(riid, out_factory);
}


const trt_named_fn k_d3d11_fns[] = {
    {"D3D11CreateDevice", (void*)&trt_D3D11CreateDevice},
    {"D3D11CreateDeviceAndSwapChain",
     (void*)&trt_D3D11CreateDeviceAndSwapChain},
};

const trt_named_fn k_dxgi_fns[] = {
    {"CreateDXGIFactory", (void*)&trt_CreateDXGIFactory},
    {"CreateDXGIFactory1", (void*)&trt_CreateDXGIFactory1},
    {"CreateDXGIFactory2", (void*)&trt_CreateDXGIFactory2},
};


void trt_gpu_set_hooks(const trt_gpu_hooks* hooks) {
  if (hooks) g_hooks = *hooks;
}

const trt_gpu_hooks* trt_gpu_hooks_get(void) { return &g_hooks; }

int trt_bridge_register_d3d(void) {
  trt_module_register("d3d11.dll", TRT_ROUTE_BUILTIN, k_d3d11_fns,
                       (int)(sizeof(k_d3d11_fns) / sizeof(k_d3d11_fns[0])));
  trt_module_register("dxgi.dll", TRT_ROUTE_BUILTIN, k_dxgi_fns,
                       (int)(sizeof(k_dxgi_fns) / sizeof(k_dxgi_fns[0])));
  return 0;
}

} // extern "C"
