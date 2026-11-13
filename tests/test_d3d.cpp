// tiramisu/tests/test_d3d.cpp
// The graphics bridge, proven offline.
//
// The host's D3D11 device/context/swapchain are fakes here (there is no GPU
// on the Linux test box), but they are REAL COM-shaped objects: the guest
// calls them through the standard vtable slots, so the test proves the
// wiring the console needs:
//
//   D3D11CreateDevice  -> the HOST's device and context
//   CreateDXGIFactory1 -> a factory that enumerates adapters and hands out
//                         the HOST's swapchain instead of failing on the
//                         guest's fake HWND
//   ... then the guest renders and presents into that swapchain
//
// The vtable slots below are the SDK's (extracted from d3d11.h/dxgi1_2.h):
// IUnknown is 3 slots, then the interface's own methods in declaration order.
#include "../src/trt/trt.h"
#include "../src/trt/bridge_d3d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
}

// ---------------- mmap backend ----------------
namespace {

struct extent { void* base; size_t len; };
extent g_extents[16];
int g_extent_count = 0;

void* linux_alloc(size_t bytes, u64 preferred_base) {
  void* p = mmap(preferred_base ? (void*)(uintptr_t)preferred_base : nullptr,
                 bytes, PROT_READ | PROT_WRITE,
                 preferred_base ? (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS)
                                : (MAP_PRIVATE | MAP_ANONYMOUS),
                 -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extent_count < 16) {
    g_extents[g_extent_count].base = p;
    g_extents[g_extent_count].len = bytes;
    ++g_extent_count;
  }
  return p;
}
int linux_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void*)b, len, p);
}
void linux_release(void* base, size_t bytes) {
  for (int i = 0; i < g_extent_count; ++i)
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
  (void)bytes;
}
void linux_log(const char* line) { printf("  [trt] %s\n", line); }

// ---------------- the fake host GPU ----------------
// Every fake object is { vtable pointer } - the COM layout the guest expects.
struct com_object { const void** vtbl; };

// what the guest actually reached
int g_created_rtv = 0;
int g_cleared = 0;
int g_drew = 0;
int g_presented = 0;
int g_enumerated_adapter = 0;

// fake object identities (the guest must land on THESE)
com_object g_device_obj = {nullptr};
com_object g_context_obj = {nullptr};
com_object g_swapchain_obj = {nullptr};
com_object g_real_factory_obj = {nullptr};
com_object g_adapter_obj = {nullptr};
com_object g_texture_obj = {nullptr};
com_object g_rtv_obj = {nullptr};

extern "C" void* g_fake_context;
extern "C" void* g_fake_device;
extern "C" void* g_fake_swapchain;
extern "C" void* g_fake_texture;
extern "C" void* g_fake_rtv;
extern "C" void* g_fake_adapter;

// generic stub: every SDK slot returns S_OK; the interesting ones are below
extern "C" TRT_GUEST_ABI int fake_generic_hr(void*, void*, void*, void*) { return 0; }

// --- device ---
extern "C" TRT_GUEST_ABI int fake_dev_CreateRenderTargetView(void* self, void* back,
                                               void* desc, void** out) {
  (void)self; (void)back; (void)desc;
  ++g_created_rtv;
  if (out) *out = g_fake_rtv;
  return 0;
}
extern "C" TRT_GUEST_ABI int fake_dev_GetImmediateContext(void* self, void** out) {
  (void)self;
  if (out) *out = g_fake_context;
  return 0;
}

// ID3D11Device: 3 (IUnknown) + 40 own methods = 43 slots
const void* g_dev_vtbl[43] = {
    [0] = (const void*)&fake_generic_hr,   // QueryInterface
    [1] = (const void*)&fake_generic_hr,   // AddRef
    [2] = (const void*)&fake_generic_hr,   // Release
    [3] = (const void*)&fake_generic_hr,   // CreateBuffer
    [4] = (const void*)&fake_generic_hr,   // CreateTexture1D
    [5] = (const void*)&fake_generic_hr,   // CreateTexture2D
    [6] = (const void*)&fake_generic_hr,   // CreateTexture3D
    [7] = (const void*)&fake_generic_hr,   // CreateShaderResourceView
    [8] = (const void*)&fake_generic_hr,   // CreateUnorderedAccessView
    [9] = (const void*)&fake_dev_CreateRenderTargetView,
    [10] = (const void*)&fake_generic_hr,  // CreateDepthStencilView
    [11] = (const void*)&fake_generic_hr,  // CreateInputLayout
    [12] = (const void*)&fake_generic_hr,  // CreateVertexShader
    [13] = (const void*)&fake_generic_hr,  // CreateGeometryShader
    [14] = (const void*)&fake_generic_hr,  // CreateGeometryShaderWithStreamOutput
    [15] = (const void*)&fake_generic_hr,  // CreatePixelShader
    [16] = (const void*)&fake_generic_hr,  // CreateHullShader
    [17] = (const void*)&fake_generic_hr,  // CreateDomainShader
    [18] = (const void*)&fake_generic_hr,  // CreateComputeShader
    [19] = (const void*)&fake_generic_hr,  // CreateClassLinkage
    [20] = (const void*)&fake_generic_hr,  // CreateBlendState
    [21] = (const void*)&fake_generic_hr,  // CreateDepthStencilState
    [22] = (const void*)&fake_generic_hr,  // CreateRasterizerState
    [23] = (const void*)&fake_generic_hr,  // CreateSamplerState
    [24] = (const void*)&fake_generic_hr,  // CreateQuery
    [25] = (const void*)&fake_generic_hr,  // CreatePredicate
    [26] = (const void*)&fake_generic_hr,  // CreateCounter
    [27] = (const void*)&fake_generic_hr,  // CreateDeferredContext
    [28] = (const void*)&fake_generic_hr,  // OpenSharedResource
    [29] = (const void*)&fake_generic_hr,  // CheckFormatSupport
    [30] = (const void*)&fake_generic_hr,  // CheckMultisampleQualityLevels
    [31] = (const void*)&fake_generic_hr,  // CheckCounterInfo
    [32] = (const void*)&fake_generic_hr,  // CheckCounter
    [33] = (const void*)&fake_generic_hr,  // CheckFeatureSupport
    [34] = (const void*)&fake_generic_hr,  // GetPrivateData
    [35] = (const void*)&fake_generic_hr,  // SetPrivateData
    [36] = (const void*)&fake_generic_hr,  // SetPrivateDataInterface
    [37] = (const void*)&fake_generic_hr,  // GetFeatureLevel
    [38] = (const void*)&fake_generic_hr,  // GetCreationFlags
    [39] = (const void*)&fake_generic_hr,  // GetDeviceRemovedReason
    [40] = (const void*)&fake_dev_GetImmediateContext,
    [41] = (const void*)&fake_generic_hr,  // SetExceptionMode
    [42] = (const void*)&fake_generic_hr,  // GetExceptionMode
};

// --- device context ---
extern "C" TRT_GUEST_ABI void fake_ctx_ClearRenderTargetView(void*, void*, const float*) {
  ++g_cleared;
}
extern "C" TRT_GUEST_ABI void fake_ctx_Draw(void*, u32, u32) { ++g_drew; }
extern "C" TRT_GUEST_ABI void fake_ctx_OMSetRenderTargets(void*, u32, void**, void**) {}
extern "C" TRT_GUEST_ABI void fake_ctx_VSSetShader(void*, void*, void**, u32) {}
extern "C" TRT_GUEST_ABI void fake_ctx_PSSetShader(void*, void*, void**, u32) {}
extern "C" TRT_GUEST_ABI void fake_ctx_IASetPrimitiveTopology(void*, u32) {}

// ID3D11DeviceContext: 3 + 112 = 115 slots
const void* g_ctx_vtbl[115] = {
    [0] = (const void*)&fake_generic_hr,
    [1] = (const void*)&fake_generic_hr,
    [2] = (const void*)&fake_generic_hr,
    [9] = (const void*)&fake_ctx_PSSetShader,           // PSSetShader
    [11] = (const void*)&fake_ctx_VSSetShader,          // VSSetShader
    [13] = (const void*)&fake_ctx_Draw,                 // Draw
    [24] = (const void*)&fake_ctx_IASetPrimitiveTopology,
    [33] = (const void*)&fake_ctx_OMSetRenderTargets,   // OMSetRenderTargets
    [50] = (const void*)&fake_ctx_ClearRenderTargetView,
};

// --- swapchain ---
extern "C" TRT_GUEST_ABI int fake_sc_GetBuffer(void* self, u32 index, const void* iid,
                                  void** out) {
  (void)self; (void)index; (void)iid;
  if (out) *out = g_fake_texture;
  return 0;
}
extern "C" TRT_GUEST_ABI int fake_sc_Present(void* self, u32 sync, u32 flags) {
  (void)self; (void)sync; (void)flags;
  ++g_presented;
  return 0;
}

// IDXGISwapChain: 3 + 15 = 18 slots
const void* g_sc_vtbl[18] = {
    [0] = (const void*)&fake_generic_hr,
    [1] = (const void*)&fake_generic_hr,
    [2] = (const void*)&fake_generic_hr,
    [8] = (const void*)&fake_sc_Present,   // Present
    [9] = (const void*)&fake_sc_GetBuffer, // GetBuffer
};

// --- the console's real factory (the one the shim forwards to) ---
extern "C" TRT_GUEST_ABI int fake_real_EnumAdapters1(void* self, u32 index, void** adapter) {
  (void)self; (void)index;
  ++g_enumerated_adapter;
  if (adapter) *adapter = g_fake_adapter;
  return 0;
}
extern "C" TRT_GUEST_ABI int fake_real_EnumAdapters(void* self, u32 index, void** adapter) {
  (void)self; (void)index;
  ++g_enumerated_adapter;
  if (adapter) *adapter = g_fake_adapter;
  return 0;
}
// IDXGIFactory2: 3 + 22 = 25 slots
const void* g_real_factory_vtbl[25] = {
    [0] = (const void*)&fake_generic_hr,
    [1] = (const void*)&fake_generic_hr,
    [2] = (const void*)&fake_generic_hr,
    [7] = (const void*)&fake_real_EnumAdapters,
    [12] = (const void*)&fake_real_EnumAdapters1,
};

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  *size = got;
  return buf;
}

u64 win64_call(void* fn, u64 a, u64 b, u64 c) {
  u64 args[3] = {a, b, c};
  u64 ret;
  asm volatile(
    "movq %[a0], %%rcx\n"
    "movq %[a1], %%rdx\n"
    "movq %[a2], %%r8\n"
    "subq $48, %%rsp\n"
    "call *%[fn]\n"
    "addq $48, %%rsp\n"
    : "=a"(ret)
    : [a0] "m"(args[0]), [a1] "m"(args[1]), [a2] "m"(args[2]),
      [fn] "r"(fn)
    : "rcx", "rdx", "r8", "r9", "r10", "r11",
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "memory", "cc");
  return ret;
}

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

} // namespace

extern "C" void* g_fake_context = (void*)&g_context_obj;
extern "C" void* g_fake_device = (void*)&g_device_obj;
extern "C" void* g_fake_swapchain = (void*)&g_swapchain_obj;
extern "C" void* g_fake_texture = (void*)&g_texture_obj;
extern "C" void* g_fake_rtv = (void*)&g_rtv_obj;
extern "C" void* g_fake_adapter = (void*)&g_adapter_obj;

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: test_d3d <guest.exe>\n");
    return 2;
  }
  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();

  // publish the "host GPU" to the bridge
  trt_gpu_hooks hooks{};
  hooks.device = g_fake_device;
  hooks.context = g_fake_context;
  hooks.swapchain = g_fake_swapchain;
  hooks.factory = &g_real_factory_obj;
  hooks.feature_level = 0xb100;  // D3D_FEATURE_LEVEL_11_0
  hooks.width = 1280;
  hooks.height = 720;
  trt_gpu_set_hooks(&hooks);

  // wire the fake COM objects to their vtables
  g_device_obj.vtbl = g_dev_vtbl;
  g_context_obj.vtbl = g_ctx_vtbl;
  g_swapchain_obj.vtbl = g_sc_vtbl;
  g_real_factory_obj.vtbl = g_real_factory_vtbl;

  printf("tiramisu D3D11/DXGI bridge test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (d3d11/dxgi imports bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s (%s)\n", trt_errstr(rc),
           img ? img->last_detail : "no image");
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0xff)\n", (unsigned long long)ret);
  CHECK(ret == 0xFFull, "guest completed the whole D3D11 frame");

  CHECK(g_enumerated_adapter == 1,
        "adapter enumeration reached the console's real factory");
  CHECK(g_created_rtv == 1, "guest created a render target on the host device");
  CHECK(g_cleared == 1, "guest cleared the host's render target");
  CHECK(g_drew == 1, "guest issued a draw on the host's context");
  CHECK(g_presented == 1,
        "guest presented into the HOST's swapchain (not its own)");

  printf("\nRESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
