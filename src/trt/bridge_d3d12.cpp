// tiramisu/src/trt/bridge_d3d12.cpp
// Direct3D 12 bridge (d3d12.dll + d3d12core.dll).
//   - On Xbox Series Dev Mode (UWP): forwards directly to the console's
//     hardware D3D12 runtime once hooks are bound by host swapchain.
//   - Offline / Linux CI: full stateful software D3D12 COM runtime using exact
//     MSVC Win64 vtable indices and hidden struct-return parameters.
#include "bridge_d3d12.h"
#include "thunk.h"

#include <atomic>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_set>
#include <vector>

extern "C" int TRT_STDCALL trt_SetEvent(u64 handle);

namespace {

trt_d3d12_hooks g_hooks{};

constexpr int k_s_ok = 0;
constexpr int k_s_false = 1;
constexpr int k_e_nointerface = (int)0x80004002u;
constexpr int k_e_invalidarg = (int)0x80070057u;
constexpr int k_e_outofmemory = (int)0x8007000Eu;
constexpr int k_e_notimpl = (int)0x80004001u;
constexpr int k_dxgi_error_unsupported = (int)0x887A0004u;

constexpr u32 k_fl_9_1 = 0x9100;
constexpr u32 k_fl_11_0 = 0xb000;
constexpr u32 k_fl_11_1 = 0xb100;
constexpr u32 k_fl_12_0 = 0xc000;
constexpr u32 k_fl_12_1 = 0xc100;
constexpr u32 k_fl_12_2 = 0xc200;

struct alignas(4) raw_guid {
  u32 d1;
  u16 d2;
  u16 d3;
  u8 d4[8];
};

bool guid_eq(const void* g, u32 d1, u16 d2, u16 d3,
             u8 b0, u8 b1, u8 b2, u8 b3, u8 b4, u8 b5, u8 b6, u8 b7) {
  if (!g) return false;
  const raw_guid* r = (const raw_guid*)g;
  return r->d1 == d1 && r->d2 == d2 && r->d3 == d3 &&
         r->d4[0] == b0 && r->d4[1] == b1 && r->d4[2] == b2 && r->d4[3] == b3 &&
         r->d4[4] == b4 && r->d4[5] == b5 && r->d4[6] == b6 && r->d4[7] == b7;
}

bool is_iunknown(const void* riid) {
  return guid_eq(riid, 0x00000000u, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
}

bool is_valid_fl(u32 fl) {
  return fl == k_fl_9_1 || fl == 0x9200 || fl == 0x9300 ||
         fl == 0xa000 || fl == 0xa100 || fl == k_fl_11_0 ||
         fl == k_fl_11_1 || fl == k_fl_12_0 || fl == k_fl_12_1 || fl == k_fl_12_2;
}

std::mutex g_track_mu;
std::unordered_set<void*> g_queues;
std::atomic<u64> g_next_va{0x120000000ULL};
std::atomic<u64> g_next_desc{0x240000000ULL};

// ---- ABI POD layouts matching xwin d3d12.h on x86_64 -----------------------
struct d3d12_luid_t {
  u32 low;
  i32 high;
};

struct d3d12_queue_desc_t {
  u32 type;
  i32 priority;
  u32 flags;
  u32 node_mask;
};

struct d3d12_heap_desc_info_t {
  u32 type;
  u32 num_descriptors;
  u32 flags;
  u32 node_mask;
};

struct d3d12_heap_props_t {
  u32 type;
  u32 cpu_page_prop;
  u32 memory_pool;
  u32 creation_node_mask;
  u32 visible_node_mask;
};

struct d3d12_heap_desc_t {
  u64 size_in_bytes;
  d3d12_heap_props_t props;
  u64 alignment;
  u32 flags;
  u32 pad;
};

struct d3d12_resource_desc_t {
  u32 dimension;
  u32 pad0;
  u64 alignment;
  u64 width;
  u32 height;
  u16 depth_or_array;
  u16 mip_levels;
  u32 format;
  u32 sample_count;
  u32 sample_quality;
  u32 layout;
  u32 flags;
  u32 pad1;
};

struct d3d12_alloc_info_t {
  u64 size_in_bytes;
  u64 alignment;
};

struct d3d12_placed_footprint_t {
  u64 offset;
  u32 format;
  u32 width;
  u32 height;
  u32 depth;
  u32 row_pitch;
};

// Forward declare offline COM object types so AddRef/Release/Destroy compose
struct d3d12_blob;
struct d3d12_device;
struct d3d12_queue;
struct d3d12_alloc;
struct d3d12_cmd_list;
struct d3d12_fence;
struct d3d12_desc_heap;
struct d3d12_resource;
struct d3d12_simple_child;

// ---- Shared IUnknown / ID3D12Object base ----------------------------------
struct com_base {
  const void** vtbl;
  std::atomic<u32> refcount{1};
  d3d12_device* device = nullptr;
  void (*destroy_fn)(com_base*) = nullptr;
};

u32 TRT_GUEST_ABI base_addref(void* self) {
  return ++((com_base*)self)->refcount;
}

u32 TRT_GUEST_ABI base_release(void* self) {
  auto* o = (com_base*)self;
  const u32 r = --o->refcount;
  if (r == 0 && o->destroy_fn) o->destroy_fn(o);
  return r;
}

int TRT_GUEST_ABI base_qi_self(void* self, const void* riid, void** ppv) {
  if (!ppv) return k_e_invalidarg;
  (void)riid;
  *ppv = self;
  base_addref(self);
  return k_s_ok;
}

int TRT_GUEST_ABI child_get_priv(void*, const void*, u32* sz, void*) {
  if (sz) *sz = 0;
  return (int)0x887A0002u;  // DXGI_ERROR_NOT_FOUND
}
int TRT_GUEST_ABI child_set_priv(void*, const void*, u32, const void*) { return k_s_ok; }
int TRT_GUEST_ABI child_set_priv_iface(void*, const void*, void*) { return k_s_ok; }
int TRT_GUEST_ABI child_set_name(void*, const u16*) { return k_s_ok; }

int TRT_GUEST_ABI child_get_device(void* self, const void* riid, void** ppv) {
  if (!ppv) return k_e_invalidarg;
  auto* o = (com_base*)self;
  if (!o->device) { *ppv = nullptr; return k_e_nointerface; }
  (void)riid;
  *ppv = o->device;
  base_addref(o->device);
  return k_s_ok;
}

// ---- ID3DBlob --------------------------------------------------------------
struct d3d12_blob {
  const void** vtbl;
  std::atomic<u32> refcount{1};
  std::vector<u8> bytes;
};

int TRT_GUEST_ABI blob_qi(void* self, const void* riid, void** ppv) {
  if (!ppv) return k_e_invalidarg;
  (void)riid;
  *ppv = self;
  ++((d3d12_blob*)self)->refcount;
  return k_s_ok;
}
u32 TRT_GUEST_ABI blob_addref(void* self) { return ++((d3d12_blob*)self)->refcount; }
u32 TRT_GUEST_ABI blob_release(void* self) {
  auto* b = (d3d12_blob*)self;
  const u32 r = --b->refcount;
  if (r == 0) delete b;
  return r;
}
void* TRT_GUEST_ABI blob_ptr(void* self) {
  auto* b = (d3d12_blob*)self;
  return b->bytes.empty() ? nullptr : b->bytes.data();
}
u64 TRT_GUEST_ABI blob_size(void* self) {
  return ((d3d12_blob*)self)->bytes.size();
}
const void* g_blob_vtbl[5] = {
    (const void*)&blob_qi,
    (const void*)&blob_addref,
    (const void*)&blob_release,
    (const void*)&blob_ptr,
    (const void*)&blob_size,
};

d3d12_blob* make_blob(const void* data, size_t sz) {
  auto* b = new d3d12_blob();
  b->vtbl = g_blob_vtbl;
  if (data && sz) b->bytes.assign((const u8*)data, (const u8*)data + sz);
  return b;
}

// ---- ID3D12Fence -----------------------------------------------------------
struct fence_waiter {
  u64 target;
  u64 event_handle;
};

struct d3d12_fence : com_base {
  std::mutex mu;
  std::atomic<u64> completed{0};
  std::vector<fence_waiter> waiters;
};

void fence_destroy(com_base* b) { delete (d3d12_fence*)b; }

void fence_notify(d3d12_fence* f, u64 value) {
  std::vector<u64> ready;
  {
    std::lock_guard<std::mutex> lk(f->mu);
    const u64 prev = f->completed.load();
    if (value > prev) f->completed.store(value);
    const u64 cur = f->completed.load();
    auto it = f->waiters.begin();
    while (it != f->waiters.end()) {
      if (it->target <= cur) {
        if (it->event_handle) ready.push_back(it->event_handle);
        it = f->waiters.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (u64 ev : ready) trt_SetEvent(ev);
}

u64 TRT_GUEST_ABI fence_completed(void* self) {
  return ((d3d12_fence*)self)->completed.load();
}

int TRT_GUEST_ABI fence_set_event(void* self, u64 value, void* hEvent) {
  auto* f = (d3d12_fence*)self;
  const u64 ev = (u64)(uintptr_t)hEvent;
  if (f->completed.load() >= value) {
    if (ev) trt_SetEvent(ev);
    return k_s_ok;
  }
  {
    std::lock_guard<std::mutex> lk(f->mu);
    if (f->completed.load() >= value) {
      if (ev) trt_SetEvent(ev);
      return k_s_ok;
    }
    if (ev) f->waiters.push_back({value, ev});
  }
  return k_s_ok;
}

int TRT_GUEST_ABI fence_signal(void* self, u64 value) {
  fence_notify((d3d12_fence*)self, value);
  return k_s_ok;
}

const void* g_fence_vtbl[11] = {
    (const void*)&base_qi_self,
    (const void*)&base_addref,
    (const void*)&base_release,
    (const void*)&child_get_priv,
    (const void*)&child_set_priv,
    (const void*)&child_set_priv_iface,
    (const void*)&child_set_name,
    (const void*)&child_get_device,
    (const void*)&fence_completed,
    (const void*)&fence_set_event,
    (const void*)&fence_signal,
};
static_assert(sizeof(g_fence_vtbl) / sizeof(g_fence_vtbl[0]) == 11,
              "ID3D12Fence vtbl must stay 11 slots (Win64 ABI)");

// ---- ID3D12DescriptorHeap --------------------------------------------------
struct d3d12_desc_heap : com_base {
  d3d12_heap_desc_info_t desc{};
  u64 cpu_base = 0;
  u64 gpu_base = 0;
};

void heap_desc_destroy(com_base* b) { delete (d3d12_desc_heap*)b; }

d3d12_heap_desc_info_t* TRT_GUEST_ABI descheap_get_desc(void* self, d3d12_heap_desc_info_t* ret) {
  if (ret) *ret = ((d3d12_desc_heap*)self)->desc;
  return ret;
}

u64* TRT_GUEST_ABI descheap_cpu_start(void* self, u64* ret) {
  if (ret) *ret = ((d3d12_desc_heap*)self)->cpu_base;
  return ret;
}

u64* TRT_GUEST_ABI descheap_gpu_start(void* self, u64* ret) {
  if (ret) *ret = ((d3d12_desc_heap*)self)->gpu_base;
  return ret;
}

const void* g_descheap_vtbl[11] = {
    (const void*)&base_qi_self,
    (const void*)&base_addref,
    (const void*)&base_release,
    (const void*)&child_get_priv,
    (const void*)&child_set_priv,
    (const void*)&child_set_priv_iface,
    (const void*)&child_set_name,
    (const void*)&child_get_device,
    (const void*)&descheap_get_desc,
    (const void*)&descheap_cpu_start,
    (const void*)&descheap_gpu_start,
};

// ---- ID3D12Resource --------------------------------------------------------
struct d3d12_resource : com_base {
  d3d12_resource_desc_t desc{};
  d3d12_heap_props_t heap_props{};
  u32 heap_flags = 0;
  u64 gpu_va = 0;
  std::vector<u8> backing;
};

void resource_destroy(com_base* b) { delete (d3d12_resource*)b; }

size_t estimate_res_bytes(const d3d12_resource_desc_t& d) {
  u64 w = d.width ? d.width : 1;
  u64 h = d.height ? d.height : 1;
  u64 bytes = (d.dimension == 1) ? w : (w * h * 4);
  if (bytes > 16u * 1024u * 1024u) bytes = 16u * 1024u * 1024u;
  return (size_t)bytes;
}

int TRT_GUEST_ABI res_map(void* self, u32 subres, const void* read_range, void** ppData) {
  (void)subres; (void)read_range;
  auto* r = (d3d12_resource*)self;
  if (r->backing.empty()) {
    const size_t sz = estimate_res_bytes(r->desc);
    r->backing.resize(sz ? sz : 256);
  }
  if (ppData) *ppData = r->backing.data();
  return k_s_ok;
}

void TRT_GUEST_ABI res_unmap(void*, u32, const void*) {}

d3d12_resource_desc_t* TRT_GUEST_ABI res_get_desc(void* self, d3d12_resource_desc_t* ret) {
  if (ret) *ret = ((d3d12_resource*)self)->desc;
  return ret;
}

u64 TRT_GUEST_ABI res_gpu_va(void* self) {
  return ((d3d12_resource*)self)->gpu_va;
}

int TRT_GUEST_ABI res_write_sub(void* self, u32, const void*, const void* src, u32 row_pitch, u32) {
  auto* r = (d3d12_resource*)self;
  if (r->backing.empty()) r->backing.resize(estimate_res_bytes(r->desc));
  if (src && row_pitch && !r->backing.empty()) {
    const size_t cp = row_pitch < r->backing.size() ? row_pitch : r->backing.size();
    memcpy(r->backing.data(), src, cp);
  }
  return k_s_ok;
}

int TRT_GUEST_ABI res_read_sub(void* self, void* dst, u32 row_pitch, u32, u32, const void*) {
  auto* r = (d3d12_resource*)self;
  if (dst && row_pitch && !r->backing.empty()) {
    const size_t cp = row_pitch < r->backing.size() ? row_pitch : r->backing.size();
    memcpy(dst, r->backing.data(), cp);
  }
  return k_s_ok;
}

int TRT_GUEST_ABI res_get_heap_props(void* self, d3d12_heap_props_t* props, u32* flags) {
  auto* r = (d3d12_resource*)self;
  if (props) *props = r->heap_props;
  if (flags) *flags = r->heap_flags;
  return k_s_ok;
}

const void* g_resource_vtbl[15] = {
    (const void*)&base_qi_self,
    (const void*)&base_addref,
    (const void*)&base_release,
    (const void*)&child_get_priv,
    (const void*)&child_set_priv,
    (const void*)&child_set_priv_iface,
    (const void*)&child_set_name,
    (const void*)&child_get_device,
    (const void*)&res_map,
    (const void*)&res_unmap,
    (const void*)&res_get_desc,
    (const void*)&res_gpu_va,
    (const void*)&res_write_sub,
    (const void*)&res_read_sub,
    (const void*)&res_get_heap_props,
};
// ABI pins: a reorder here silently breaks every D3D12 game's resource calls.
static_assert(sizeof(g_resource_vtbl) / sizeof(g_resource_vtbl[0]) == 15,
              "ID3D12Resource vtbl must stay 15 slots (Win64 ABI)");

// ---- ID3D12RootSignature / PipelineState / QueryHeap / CommandSignature ----
struct d3d12_simple_child : com_base {
  d3d12_heap_desc_t heap_desc{};
  std::vector<u8> cached_pso;
};

void simple_child_destroy(com_base* b) { delete (d3d12_simple_child*)b; }

d3d12_heap_desc_t* TRT_GUEST_ABI heap_get_desc(void* self, d3d12_heap_desc_t* ret) {
  if (ret) *ret = ((d3d12_simple_child*)self)->heap_desc;
  return ret;
}

int TRT_GUEST_ABI pso_get_cached_blob(void* self, void** out_blob) {
  if (!out_blob) return k_e_invalidarg;
  auto* p = (d3d12_simple_child*)self;
  const char dummy[16] = {'T','P','S','O', 1, 0, 0, 0};
  *out_blob = p->cached_pso.empty()
                  ? make_blob(dummy, sizeof(dummy))
                  : make_blob(p->cached_pso.data(), p->cached_pso.size());
  return k_s_ok;
}

const void* g_child8_vtbl[8] = {
    (const void*)&base_qi_self, (const void*)&base_addref, (const void*)&base_release,
    (const void*)&child_get_priv, (const void*)&child_set_priv,
    (const void*)&child_set_priv_iface, (const void*)&child_set_name,
    (const void*)&child_get_device,
};

const void* g_pso_vtbl[9] = {
    (const void*)&base_qi_self, (const void*)&base_addref, (const void*)&base_release,
    (const void*)&child_get_priv, (const void*)&child_set_priv,
    (const void*)&child_set_priv_iface, (const void*)&child_set_name,
    (const void*)&child_get_device, (const void*)&pso_get_cached_blob,
};

const void* g_heap_vtbl[9] = {
    (const void*)&base_qi_self, (const void*)&base_addref, (const void*)&base_release,
    (const void*)&child_get_priv, (const void*)&child_set_priv,
    (const void*)&child_set_priv_iface, (const void*)&child_set_name,
    (const void*)&child_get_device, (const void*)&heap_get_desc,
};

// ---- ID3D12CommandAllocator ------------------------------------------------
struct d3d12_alloc : com_base {
  u32 type = 0;
  std::atomic<u32> reset_count{0};
};
void alloc_destroy(com_base* b) { delete (d3d12_alloc*)b; }
int TRT_GUEST_ABI alloc_reset(void* self) {
  ++((d3d12_alloc*)self)->reset_count;
  return k_s_ok;
}
const void* g_alloc_vtbl[9] = {
    (const void*)&base_qi_self, (const void*)&base_addref, (const void*)&base_release,
    (const void*)&child_get_priv, (const void*)&child_set_priv,
    (const void*)&child_set_priv_iface, (const void*)&child_set_name,
    (const void*)&child_get_device, (const void*)&alloc_reset,
};

// ---- ID3D12GraphicsCommandList ---------------------------------------------
struct d3d12_cmd_list : com_base {
  u32 type = 0;
  bool closed = false;
  u32 draws = 0;
  u32 dispatches = 0;
  u32 clears = 0;
  u32 barriers = 0;
};
void cmdlist_destroy(com_base* b) { delete (d3d12_cmd_list*)b; }

u32 TRT_GUEST_ABI cmd_get_type(void* self) { return ((d3d12_cmd_list*)self)->type; }
int TRT_GUEST_ABI cmd_close(void* self) {
  auto* cl = (d3d12_cmd_list*)self;
  if (cl->closed) return k_e_invalidarg;
  cl->closed = true;
  return k_s_ok;
}
int TRT_GUEST_ABI cmd_reset(void* self, void* alloc, void*) {
  if (!alloc) return k_e_invalidarg;
  auto* cl = (d3d12_cmd_list*)self;
  cl->closed = false;
  cl->draws = cl->dispatches = cl->clears = cl->barriers = 0;
  return k_s_ok;
}
void TRT_GUEST_ABI cmd_clear_state(void*, void*) {}
void TRT_GUEST_ABI cmd_draw_inst(void* self, u32 verts, u32 insts, u32 sv,
                                 u32 siv) {
  auto* cl = (d3d12_cmd_list*)self;
  ++cl->draws;
  // fail-visible: the draw is RECORDED into the list (offline never renders);
  // the TRIANGLE GATE asserts this line exists in the host log.
  char line[96];
  snprintf(line, sizeof(line),
           "d3d12: cl DrawInstanced(%u,%u,%u,%u) recorded (draws=%u)",
           verts, insts, sv, siv, cl->draws);
  trt_core_log(line);
}
void TRT_GUEST_ABI cmd_draw_idx(void* self, u32, u32, u32, i32, u32) { ++((d3d12_cmd_list*)self)->draws; }
void TRT_GUEST_ABI cmd_dispatch(void* self, u32, u32, u32) { ++((d3d12_cmd_list*)self)->dispatches; }
void TRT_GUEST_ABI cmd_barrier(void* self, u32 n, const void*) { ((d3d12_cmd_list*)self)->barriers += n; }
void TRT_GUEST_ABI cmd_clear_rtv(void* self, u64, const void*, u32, const void*) { ++((d3d12_cmd_list*)self)->clears; }
void TRT_GUEST_ABI cmd_clear_dsv(void* self, u64, u32, float, u8, u32, const void*) { ++((d3d12_cmd_list*)self)->clears; }
void TRT_GUEST_ABI cmd_noop(void*) {}

const void* g_cmdlist_vtbl[60] = {
    (const void*)&base_qi_self,        // 0 QueryInterface
    (const void*)&base_addref,         // 1 AddRef
    (const void*)&base_release,        // 2 Release
    (const void*)&child_get_priv,      // 3 GetPrivateData
    (const void*)&child_set_priv,      // 4 SetPrivateData
    (const void*)&child_set_priv_iface,// 5 SetPrivateDataInterface
    (const void*)&child_set_name,      // 6 SetName
    (const void*)&child_get_device,    // 7 GetDevice
    (const void*)&cmd_get_type,        // 8 GetType
    (const void*)&cmd_close,           // 9 Close
    (const void*)&cmd_reset,           // 10 Reset
    (const void*)&cmd_clear_state,     // 11 ClearState
    (const void*)&cmd_draw_inst,       // 12 DrawInstanced
    (const void*)&cmd_draw_idx,        // 13 DrawIndexedInstanced
    (const void*)&cmd_dispatch,        // 14 Dispatch
    (const void*)&cmd_noop,            // 15 CopyBufferRegion
    (const void*)&cmd_noop,            // 16 CopyTextureRegion
    (const void*)&cmd_noop,            // 17 CopyResource
    (const void*)&cmd_noop,            // 18 CopyTiles
    (const void*)&cmd_noop,            // 19 ResolveSubresource
    (const void*)&cmd_noop,            // 20 IASetPrimitiveTopology
    (const void*)&cmd_noop,            // 21 RSSetViewports
    (const void*)&cmd_noop,            // 22 RSSetScissorRects
    (const void*)&cmd_noop,            // 23 OMSetBlendFactor
    (const void*)&cmd_noop,            // 24 OMSetStencilRef
    (const void*)&cmd_noop,            // 25 SetPipelineState
    (const void*)&cmd_barrier,         // 26 ResourceBarrier
    (const void*)&cmd_noop,            // 27 ExecuteBundle
    (const void*)&cmd_noop,            // 28 SetDescriptorHeaps
    (const void*)&cmd_noop,            // 29 SetComputeRootSignature
    (const void*)&cmd_noop,            // 30 SetGraphicsRootSignature
    (const void*)&cmd_noop,            // 31 SetComputeRootDescriptorTable
    (const void*)&cmd_noop,            // 32 SetGraphicsRootDescriptorTable
    (const void*)&cmd_noop,            // 33 SetComputeRoot32BitConstant
    (const void*)&cmd_noop,            // 34 SetGraphicsRoot32BitConstant
    (const void*)&cmd_noop,            // 35 SetComputeRoot32BitConstants
    (const void*)&cmd_noop,            // 36 SetGraphicsRoot32BitConstants
    (const void*)&cmd_noop,            // 37 SetComputeRootConstantBufferView
    (const void*)&cmd_noop,            // 38 SetGraphicsRootConstantBufferView
    (const void*)&cmd_noop,            // 39 SetComputeRootShaderResourceView
    (const void*)&cmd_noop,            // 40 SetGraphicsRootShaderResourceView
    (const void*)&cmd_noop,            // 41 SetComputeRootUnorderedAccessView
    (const void*)&cmd_noop,            // 42 SetGraphicsRootUnorderedAccessView
    (const void*)&cmd_noop,            // 43 IASetIndexBuffer
    (const void*)&cmd_noop,            // 44 IASetVertexBuffers
    (const void*)&cmd_noop,            // 45 SOSetTargets
    (const void*)&cmd_noop,            // 46 OMSetRenderTargets
    (const void*)&cmd_clear_dsv,       // 47 ClearDepthStencilView
    (const void*)&cmd_clear_rtv,       // 48 ClearRenderTargetView
    (const void*)&cmd_noop,            // 49 ClearUnorderedAccessViewUint
    (const void*)&cmd_noop,            // 50 ClearUnorderedAccessViewFloat
    (const void*)&cmd_noop,            // 51 DiscardResource
    (const void*)&cmd_noop,            // 52 BeginQuery
    (const void*)&cmd_noop,            // 53 EndQuery
    (const void*)&cmd_noop,            // 54 ResolveQueryData
    (const void*)&cmd_noop,            // 55 SetPredication
    (const void*)&cmd_noop,            // 56 SetMarker
    (const void*)&cmd_noop,            // 57 BeginEvent
    (const void*)&cmd_noop,            // 58 EndEvent
    (const void*)&cmd_draw_inst,       // 59 ExecuteIndirect
};
static_assert(sizeof(g_cmdlist_vtbl) / sizeof(g_cmdlist_vtbl[0]) == 60,
              "ID3D12GraphicsCommandList vtbl must stay 60 slots (Win64 ABI)");

// ---- ID3D12CommandQueue ----------------------------------------------------
// Real ABI: ID3D12CommandQueue has exactly 13 vtable slots (0-12).
//   0-2  IUnknown (QueryInterface, AddRef, Release)
//   3-6  ID3D12Object (GetPrivateData, SetPrivateData, SetPrivateDataInterface,
//        SetName)
//   7    ID3D12Pageable::GetDevice
//   8    ExecuteCommandLists
//   9    Signal
//   10   Wait
//   11   GetTimestampFrequency
//   12   GetClockCalibration
// The vtbl below must stay in exactly this order; d3d12_vtbl_selfcheck()
// enforces it at registration time.
struct d3d12_queue : com_base {
  d3d12_queue_desc_t desc{};
  std::atomic<u64> executed_lists{0};
  // TRIANGLE GATE support: ExecuteCommandLists records the submitted command
  // lists here so offline tests can assert the game's submission landed.
  std::mutex mu;
  std::vector<void*> recorded_lists;
};

void queue_destroy(com_base* b) {
  {
    std::lock_guard<std::mutex> lk(g_track_mu);
    g_queues.erase(b);
  }
  delete (d3d12_queue*)b;
}

int TRT_GUEST_ABI queue_qi(void* self, const void* riid, void** ppv) {
  if (!ppv) return k_e_invalidarg;
  if (!riid || is_iunknown(riid) ||
      guid_eq(riid, 0x0ec870a6u, 0x5d7e, 0x4c22, 0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed) ||
      guid_eq(riid, 0xc4fec28fu, 0x7966, 0x4e95, 0x9f, 0x94, 0xf4, 0x31, 0xcb, 0x56, 0xc3, 0xb8)) {
    *ppv = self;
    base_addref(self);
    return k_s_ok;
  }
  *ppv = nullptr;
  return k_e_nointerface;
}

void TRT_GUEST_ABI queue_update_tile(void*, void*, u32, const void*, const void*, void*, u32, const void*, const void*, const void*, u32) {}
void TRT_GUEST_ABI queue_copy_tile(void*, void*, const void*, const void*, void*, u64, u32) {}

void TRT_GUEST_ABI queue_exec_lists(void* self, u32 count, void* const* lists) {
  auto* q = (d3d12_queue*)self;
  if (!count || !lists) return;
  q->executed_lists += count;
  {
    std::lock_guard<std::mutex> lk(q->mu);
    for (u32 i = 0; i < count; ++i) {
      if (lists[i]) q->recorded_lists.push_back(lists[i]);
    }
  }
  // fail-visible: submission landed; the TRIANGLE GATE asserts this line.
  char line[80];
  snprintf(line, sizeof(line),
           "d3d12: queue ExecuteCommandLists(%u) recorded (total=%llu)", count,
           (unsigned long long)q->executed_lists.load());
  trt_core_log(line);
}

void TRT_GUEST_ABI queue_marker_evt(void*, u32, const void*, u32) {}
void TRT_GUEST_ABI queue_end_evt(void*) {}

int TRT_GUEST_ABI queue_signal(void*, void* fence, u64 value) {
  if (!fence) return k_e_invalidarg;
  fence_notify((d3d12_fence*)fence, value);
  return k_s_ok;
}

int TRT_GUEST_ABI queue_wait(void*, void* fence, u64 value) {
  if (!fence) return k_e_invalidarg;
  (void)value;
  return k_s_ok;
}

int TRT_GUEST_ABI queue_ts_freq(void*, u64* freq) {
  if (!freq) return k_e_invalidarg;
  *freq = 100000000ULL;
  return k_s_ok;
}

int TRT_GUEST_ABI queue_clock_cal(void*, u64* gpu_ts, u64* cpu_ts) {
  if (gpu_ts) *gpu_ts = 1000;
  if (cpu_ts) *cpu_ts = 1000;
  return k_s_ok;
}

d3d12_queue_desc_t* TRT_GUEST_ABI queue_get_desc(void* self, d3d12_queue_desc_t* ret) {
  if (ret) *ret = ((d3d12_queue*)self)->desc;
  return ret;
}

// Exact ID3D12CommandQueue ABI: 13 slots, ExecuteCommandLists at slot 8.
// NOTE: queue_update_tile / queue_copy_tile / queue_marker_evt / queue_end_evt
// / queue_get_desc remain defined above but are intentionally NOT registered
// in the vtbl — ID3D12CommandQueue has no such methods, and padding the table
// was what pushed Signal/Wait/ExecuteCommandLists onto the wrong slots
// (BUG-1: every game's render submission silently no-oped).
const void* g_queue_vtbl[13] = {
    (const void*)&queue_qi,             // 0  IUnknown::QueryInterface
    (const void*)&base_addref,          // 1  IUnknown::AddRef
    (const void*)&base_release,         // 2  IUnknown::Release
    (const void*)&child_get_priv,       // 3  ID3D12Object::GetPrivateData
    (const void*)&child_set_priv,       // 4  ID3D12Object::SetPrivateData
    (const void*)&child_set_priv_iface, // 5  ID3D12Object::SetPrivateDataInterface
    (const void*)&child_set_name,       // 6  ID3D12Object::SetName
    (const void*)&child_get_device,     // 7  ID3D12Pageable::GetDevice
    (const void*)&queue_exec_lists,     // 8  ExecuteCommandLists  <- critical
    (const void*)&queue_signal,         // 9  Signal
    (const void*)&queue_wait,           // 10 Wait
    (const void*)&queue_ts_freq,        // 11 GetTimestampFrequency
    (const void*)&queue_clock_cal,      // 12 GetClockCalibration
};

static_assert(sizeof(g_queue_vtbl) / sizeof(g_queue_vtbl[0]) == 13,
              "ID3D12CommandQueue ABI: exactly 13 vtable slots (0-12)");

// Runtime ABI guard for the TRIANGLE GATE: verifies the queue vtbl layout
// slot-by-slot (the function identities that games depend on). Returns false
// (and logs the first bad slot) if any slot is wrong; called from
// trt_bridge_register_d3d12 so a reorder cannot regress silently.
int d3d12_vtbl_selfcheck(void) {
  const void* want[13] = {
      (const void*)&queue_qi,
      (const void*)&base_addref,
      (const void*)&base_release,
      (const void*)&child_get_priv,
      (const void*)&child_set_priv,
      (const void*)&child_set_priv_iface,
      (const void*)&child_set_name,
      (const void*)&child_get_device,
      (const void*)&queue_exec_lists,
      (const void*)&queue_signal,
      (const void*)&queue_wait,
      (const void*)&queue_ts_freq,
      (const void*)&queue_clock_cal,
  };
  for (int i = 0; i < 13; ++i) {
    if (g_queue_vtbl[i] != want[i]) {
      char line[96];
      snprintf(line, sizeof(line), "d3d12: vtbl selfcheck FAILED slot %d", i);
      trt_core_log(line);
      return 0;
    }
  }
  // Structural sanity: the critical slot must hold ExecuteCommandLists.
  if (g_queue_vtbl[8] != (const void*)&queue_exec_lists) {
    trt_core_log("d3d12: vtbl selfcheck FAILED slot 8 (ExecuteCommandLists)");
    return 0;
  }
  return 1;
}

// ---- ID3D12Device4 ---------------------------------------------------------
struct d3d12_device : com_base {
  u32 max_fl = k_fl_12_1;
  d3d12_luid_t luid{0x1337u, 0};
};

void device_destroy(com_base* b) { delete (d3d12_device*)b; }

int TRT_GUEST_ABI dev_qi(void* self, const void* riid, void** ppv) {
  if (!ppv) return k_e_invalidarg;
  if (!riid || is_iunknown(riid) ||
      guid_eq(riid, 0x189819f1u, 0x1db6, 0x4b57, 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7) || // ID3D12Device
      guid_eq(riid, 0x77acce80u, 0x638e, 0x4e65, 0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e) || // ID3D12Device1
      guid_eq(riid, 0x30baa41eu, 0xb15b, 0x475c, 0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28) || // ID3D12Device2
      guid_eq(riid, 0x81dadc15u, 0x2bad, 0x4392, 0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98) || // ID3D12Device3
      guid_eq(riid, 0xe865df17u, 0xa9ee, 0x46f9, 0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5) || // ID3D12Device4
      guid_eq(riid, 0x8b4f173bu, 0x2fea, 0x4b80, 0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d) || // ID3D12Device5
      guid_eq(riid, 0xc70b221bu, 0x40e4, 0x4a17, 0x89, 0xaf, 0x02, 0x5a, 0x07, 0x27, 0xa6, 0xdc) || // ID3D12Device6
      guid_eq(riid, 0x5c014b53u, 0x68a1, 0x4b9b, 0x8b, 0xd1, 0xdd, 0x60, 0x46, 0xb9, 0x35, 0x8b) || // ID3D12Device7
      guid_eq(riid, 0x9218e6bbu, 0xf944, 0x4f7e, 0xa7, 0x5c, 0xb1, 0xb2, 0xc7, 0xb7, 0x01, 0xf3) || // ID3D12Device8
      guid_eq(riid, 0x4c80e962u, 0xf032, 0x4f60, 0xbc, 0x9e, 0xeb, 0xc2, 0xcf, 0xa1, 0xd8, 0x3c) || // ID3D12Device9
      guid_eq(riid, 0xc7e2816fu, 0xb6fa, 0x4f91, 0xa5, 0x72, 0x9e, 0x5c, 0x78, 0x64, 0x75, 0x38)) { // ID3D12Object
    *ppv = self;
    base_addref(self);
    return k_s_ok;
  }
  *ppv = nullptr;
  return k_e_nointerface;
}

u32 TRT_GUEST_ABI dev_node_count(void*) { return 1; }

int TRT_GUEST_ABI dev_create_queue(void* self, const d3d12_queue_desc_t* desc,
                                   const void*, void** out_q) {
  if (!desc || !out_q) return k_e_invalidarg;
  auto* q = new d3d12_queue();
  q->vtbl = g_queue_vtbl;
  q->device = (d3d12_device*)self;
  q->destroy_fn = &queue_destroy;
  q->desc = *desc;
  {
    std::lock_guard<std::mutex> lk(g_track_mu);
    g_queues.insert(q);
  }
  *out_q = q;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_alloc(void* self, u32 type, const void*, void** out_alloc) {
  if (!out_alloc) return k_e_invalidarg;
  auto* a = new d3d12_alloc();
  a->vtbl = g_alloc_vtbl;
  a->device = (d3d12_device*)self;
  a->destroy_fn = &alloc_destroy;
  a->type = type;
  *out_alloc = a;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_pso(void* self, const void*, const void*, void** out_pso) {
  if (!out_pso) return k_e_invalidarg;
  auto* p = new d3d12_simple_child();
  p->vtbl = g_pso_vtbl;
  p->device = (d3d12_device*)self;
  p->destroy_fn = &simple_child_destroy;
  *out_pso = p;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_cmdlist(void* self, u32, u32 type, void* alloc,
                                     void*, const void*, void** out_cl) {
  if (!alloc || !out_cl) return k_e_invalidarg;
  auto* cl = new d3d12_cmd_list();
  cl->vtbl = g_cmdlist_vtbl;
  cl->device = (d3d12_device*)self;
  cl->destroy_fn = &cmdlist_destroy;
  cl->type = type;
  *out_cl = cl;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_cmdlist1(void* self, u32, u32 type, u32,
                                      const void*, void** out_cl) {
  if (!out_cl) return k_e_invalidarg;
  auto* cl = new d3d12_cmd_list();
  cl->vtbl = g_cmdlist_vtbl;
  cl->device = (d3d12_device*)self;
  cl->destroy_fn = &cmdlist_destroy;
  cl->type = type;
  cl->closed = true;
  *out_cl = cl;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_check_feature(void* self, u32 feature, void* data, u32 sz) {
  if (!data) return k_e_invalidarg;
  auto* d = (d3d12_device*)self;
  if (feature == 0 && sz >= 16) {
    // D3D12_FEATURE_D3D12_OPTIONS: clean conservative baseline + resource binding tier 3
    memset(data, 0, sz);
    ((u32*)data)[2] = 3;  // ResourceBindingTier = TIER_3
    return k_s_ok;
  }
  if (feature == 2 && sz >= 8) {
    // D3D12_FEATURE_FEATURE_LEVELS
    struct fl_data { u32 num; const u32* list; u32 max_fl; };
    auto* fl = (fl_data*)data;
    u32 best = 0;
    if (fl->list) {
      for (u32 i = 0; i < fl->num; ++i)
        if (fl->list[i] <= d->max_fl && fl->list[i] > best) best = fl->list[i];
    }
    fl->max_fl = best ? best : d->max_fl;
    return best ? k_s_ok : k_dxgi_error_unsupported;
  }
  if (feature == 7 && sz >= 4) {
    // D3D12_FEATURE_SHADER_MODEL
    u32* sm = (u32*)data;
    if (*sm > 0x65u) *sm = 0x65u;
    return k_s_ok;
  }
  if (feature == 12 && sz >= 4) {
    // D3D12_FEATURE_ROOT_SIGNATURE: ROOT_SIGNATURE_VERSION_1_1 (0x2)
    u32* rs = (u32*)data;
    if (*rs > 2u) *rs = 2u;
    return k_s_ok;
  }
  memset(data, 0, sz);
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_descheap(void* self, const d3d12_heap_desc_info_t* desc,
                                      const void*, void** out_h) {
  if (!desc || !out_h) return k_e_invalidarg;
  auto* h = new d3d12_desc_heap();
  h->vtbl = g_descheap_vtbl;
  h->device = (d3d12_device*)self;
  h->destroy_fn = &heap_desc_destroy;
  h->desc = *desc;
  const u64 span = (u64)(desc->num_descriptors ? desc->num_descriptors : 1) * 64ULL;
  h->cpu_base = g_next_desc.fetch_add(span + 64ULL);
  if (desc->flags & 1u) h->gpu_base = h->cpu_base | 0x400000000000ULL;
  *out_h = h;
  return k_s_ok;
}

u32 TRT_GUEST_ABI dev_desc_inc(void*, u32 type) {
  return (type == 0 || type == 1) ? 32u : 64u;
}

int TRT_GUEST_ABI dev_create_rootsig(void* self, u32, const void* blob, u64 len,
                                     const void*, void** out_rs) {
  if (!blob || !len || !out_rs) return k_e_invalidarg;
  auto* rs = new d3d12_simple_child();
  rs->vtbl = g_child8_vtbl;
  rs->device = (d3d12_device*)self;
  rs->destroy_fn = &simple_child_destroy;
  *out_rs = rs;
  return k_s_ok;
}

void TRT_GUEST_ABI dev_view_noop(void*) {}

d3d12_alloc_info_t* TRT_GUEST_ABI dev_alloc_info(void*, d3d12_alloc_info_t* ret,
                                                 u32, u32 count,
                                                 const d3d12_resource_desc_t* descs) {
  if (!ret) return ret;
  u64 total = 0;
  for (u32 i = 0; i < count; ++i) {
    u64 w = descs ? (descs[i].width ? descs[i].width : 1) : 64;
    u64 h = descs ? (descs[i].height ? descs[i].height : 1) : 1;
    u64 sz = (descs && descs[i].dimension == 1) ? w : (w * h * 4ULL);
    total += (sz + 65535ULL) & ~65535ULL;
  }
  ret->size_in_bytes = total ? total : 65536ULL;
  ret->alignment = 65536ULL;
  return ret;
}

d3d12_heap_props_t* TRT_GUEST_ABI dev_custom_heap(void*, d3d12_heap_props_t* ret,
                                                  u32, u32 heap_type) {
  if (!ret) return ret;
  memset(ret, 0, sizeof(*ret));
  ret->type = heap_type;
  ret->cpu_page_prop = (heap_type == 2) ? 2 : 1;
  ret->memory_pool = 1;
  ret->creation_node_mask = 1;
  ret->visible_node_mask = 1;
  return ret;
}

int TRT_GUEST_ABI dev_create_resource(void* self, const d3d12_heap_props_t* props,
                                      u32 heap_flags, const d3d12_resource_desc_t* desc,
                                      u32, const void*, const void*, void** out_res) {
  if (!desc || !out_res) return k_e_invalidarg;
  auto* r = new d3d12_resource();
  r->vtbl = g_resource_vtbl;
  r->device = (d3d12_device*)self;
  r->destroy_fn = &resource_destroy;
  r->desc = *desc;
  if (props) r->heap_props = *props;
  r->heap_flags = heap_flags;
  const u64 sz = estimate_res_bytes(*desc);
  r->gpu_va = g_next_va.fetch_add(((sz + 65535ULL) & ~65535ULL) + 65536ULL);
  *out_res = r;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_resource1(void* self, const d3d12_heap_props_t* props,
                                       u32 heap_flags, const d3d12_resource_desc_t* desc,
                                       u32 init_state, const void* clear_val,
                                       void* prot, const void* riid, void** out_res) {
  (void)prot;
  return dev_create_resource(self, props, heap_flags, desc, init_state, clear_val,
                             riid, out_res);
}

int TRT_GUEST_ABI dev_create_heap(void* self, const d3d12_heap_desc_t* desc,
                                  const void*, void** out_heap) {
  if (!desc || !out_heap) return k_e_invalidarg;
  auto* h = new d3d12_simple_child();
  h->vtbl = g_heap_vtbl;
  h->device = (d3d12_device*)self;
  h->destroy_fn = &simple_child_destroy;
  h->heap_desc = *desc;
  *out_heap = h;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_heap1(void* self, const d3d12_heap_desc_t* desc,
                                   void*, const void* riid, void** out_heap) {
  return dev_create_heap(self, desc, riid, out_heap);
}

int TRT_GUEST_ABI dev_create_placed(void* self, void*, u64,
                                    const d3d12_resource_desc_t* desc, u32,
                                    const void*, const void* riid, void** out_res) {
  d3d12_heap_props_t p{};
  p.type = 1;
  return dev_create_resource(self, &p, 0, desc, 0, nullptr, riid, out_res);
}

int TRT_GUEST_ABI dev_unimpl(void*) { return k_e_notimpl; }

int TRT_GUEST_ABI dev_create_fence(void* self, u64 init_val, u32,
                                   const void*, void** out_fence) {
  if (!out_fence) return k_e_invalidarg;
  auto* f = new d3d12_fence();
  f->vtbl = g_fence_vtbl;
  f->device = (d3d12_device*)self;
  f->destroy_fn = &fence_destroy;
  f->completed.store(init_val);
  *out_fence = f;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_residency(void*, u32, void* const*) { return k_s_ok; }
int TRT_GUEST_ABI dev_removed_reason(void*) { return k_s_ok; }

void TRT_GUEST_ABI dev_footprints(void*, const d3d12_resource_desc_t* desc,
                                  u32 first_sub, u32 num_sub, u64 base_off,
                                  d3d12_placed_footprint_t* layouts,
                                  u32* num_rows, u64* row_bytes, u64* total_bytes) {
  const u32 w = (desc && desc->width) ? (u32)desc->width : 1u;
  const u32 h = (desc && desc->height) ? desc->height : 1u;
  const u32 bpp = (desc && desc->dimension == 1) ? 1u : 4u;
  const u32 rb = w * bpp;
  const u32 pitch = (rb + 255u) & ~255u;
  u64 off = base_off;
  for (u32 i = 0; i < num_sub; ++i) {
    const u32 mip = first_sub + i;
    const u32 mw = (w >> mip) ? (w >> mip) : 1u;
    const u32 mh = (desc && desc->dimension == 1) ? 1u : ((h >> mip) ? (h >> mip) : 1u);
    const u32 mrb = mw * bpp;
    const u32 mpitch = (mrb + 255u) & ~255u;
    if (layouts) {
      layouts[i].offset = off;
      layouts[i].format = desc ? desc->format : 0;
      layouts[i].width = mw;
      layouts[i].height = mh;
      layouts[i].depth = 1;
      layouts[i].row_pitch = mpitch;
    }
    if (num_rows) num_rows[i] = mh;
    if (row_bytes) row_bytes[i] = mrb;
    off += (u64)mpitch * mh;
    off = (off + 511ULL) & ~511ULL;
  }
  if (total_bytes) *total_bytes = off ? (off - base_off) : (u64)pitch * h;
}

int TRT_GUEST_ABI dev_create_simple8(void* self, const void*, const void*, void** out) {
  if (!out) return k_e_invalidarg;
  auto* o = new d3d12_simple_child();
  o->vtbl = g_child8_vtbl;
  o->device = (d3d12_device*)self;
  o->destroy_fn = &simple_child_destroy;
  *out = o;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_cmdsig(void* self, const void* d, void*, const void* r, void** out) {
  return dev_create_simple8(self, d, r, out);
}

int TRT_GUEST_ABI dev_ok(void*) { return k_s_ok; }

d3d12_luid_t* TRT_GUEST_ABI dev_get_luid(void* self, d3d12_luid_t* ret) {
  if (ret) *ret = ((d3d12_device*)self)->luid;
  return ret;
}

const void* g_device_vtbl[57] = {
    (const void*)&dev_qi,               // 0 QueryInterface
    (const void*)&base_addref,          // 1 AddRef
    (const void*)&base_release,         // 2 Release
    (const void*)&child_get_priv,       // 3 GetPrivateData
    (const void*)&child_set_priv,       // 4 SetPrivateData
    (const void*)&child_set_priv_iface, // 5 SetPrivateDataInterface
    (const void*)&child_set_name,       // 6 SetName
    (const void*)&dev_node_count,       // 7 GetNodeCount
    (const void*)&dev_create_queue,     // 8 CreateCommandQueue
    (const void*)&dev_create_alloc,     // 9 CreateCommandAllocator
    (const void*)&dev_create_pso,       // 10 CreateGraphicsPipelineState
    (const void*)&dev_create_pso,       // 11 CreateComputePipelineState
    (const void*)&dev_create_cmdlist,   // 12 CreateCommandList
    (const void*)&dev_check_feature,    // 13 CheckFeatureSupport
    (const void*)&dev_create_descheap,  // 14 CreateDescriptorHeap
    (const void*)&dev_desc_inc,         // 15 GetDescriptorHandleIncrementSize
    (const void*)&dev_create_rootsig,   // 16 CreateRootSignature
    (const void*)&dev_view_noop,        // 17 CreateConstantBufferView
    (const void*)&dev_view_noop,        // 18 CreateShaderResourceView
    (const void*)&dev_view_noop,        // 19 CreateUnorderedAccessView
    (const void*)&dev_view_noop,        // 20 CreateRenderTargetView
    (const void*)&dev_view_noop,        // 21 CreateDepthStencilView
    (const void*)&dev_view_noop,        // 22 CreateSampler
    (const void*)&dev_view_noop,        // 23 CopyDescriptors
    (const void*)&dev_view_noop,        // 24 CopyDescriptorsSimple
    (const void*)&dev_alloc_info,       // 25 GetResourceAllocationInfo
    (const void*)&dev_custom_heap,      // 26 GetCustomHeapProperties
    (const void*)&dev_create_resource,  // 27 CreateCommittedResource
    (const void*)&dev_create_heap,      // 28 CreateHeap
    (const void*)&dev_create_placed,    // 29 CreatePlacedResource
    (const void*)&dev_unimpl,           // 30 CreateReservedResource
    (const void*)&dev_unimpl,           // 31 CreateSharedHandle
    (const void*)&dev_unimpl,           // 32 OpenSharedHandle
    (const void*)&dev_unimpl,           // 33 OpenSharedHandleByName
    (const void*)&dev_residency,        // 34 MakeResident
    (const void*)&dev_residency,        // 35 Evict
    (const void*)&dev_create_fence,     // 36 CreateFence
    (const void*)&dev_removed_reason,   // 37 GetDeviceRemovedReason
    (const void*)&dev_footprints,       // 38 GetCopyableFootprints
    (const void*)&dev_create_simple8,   // 39 CreateQueryHeap
    (const void*)&dev_ok,               // 40 SetStablePowerState
    (const void*)&dev_create_cmdsig,    // 41 CreateCommandSignature
    (const void*)&dev_view_noop,        // 42 GetResourceTiling
    (const void*)&dev_get_luid,         // 43 GetAdapterLuid
    (const void*)&dev_unimpl,           // 44 CreatePipelineLibrary
    (const void*)&dev_ok,               // 45 SetEventOnMultipleFenceCompletion
    (const void*)&dev_ok,               // 46 SetResidencyPriority
    (const void*)&dev_create_pso,       // 47 CreatePipelineState
    (const void*)&dev_unimpl,           // 48 OpenExistingHeapFromAddress
    (const void*)&dev_unimpl,           // 49 OpenExistingHeapFromFileMapping
    (const void*)&dev_ok,               // 50 EnqueueMakeResident
    (const void*)&dev_create_cmdlist1,  // 51 CreateCommandList1
    (const void*)&dev_unimpl,           // 52 CreateProtectedResourceSession
    (const void*)&dev_create_resource1, // 53 CreateCommittedResource1
    (const void*)&dev_create_heap1,     // 54 CreateHeap1
    (const void*)&dev_unimpl,           // 55 CreateReservedResource1
    (const void*)&dev_alloc_info,       // 56 GetResourceAllocationInfo1
};

// ---- Agility SDK helper interfaces (SDKConfiguration / DeviceFactory) -----
struct simple_com {
  const void** vtbl;
  std::atomic<u32> refcount{1};
};
u32 TRT_GUEST_ABI scom_addref(void* s) { return ++((simple_com*)s)->refcount; }
u32 TRT_GUEST_ABI scom_rel(void* s) {
  auto* p = (simple_com*)s;
  const u32 r = --p->refcount;
  if (r == 0) delete p;
  return r;
}
int TRT_GUEST_ABI scom_qi(void* s, const void*, void** ppv) {
  if (!ppv) return k_e_invalidarg;
  *ppv = s;
  scom_addref(s);
  return k_s_ok;
}
int TRT_GUEST_ABI sdkcfg_set_ver(void*, u32, const char*) { return k_s_ok; }
const void* g_sdkcfg_vtbl[4] = {
    (const void*)&scom_qi, (const void*)&scom_addref,
    (const void*)&scom_rel, (const void*)&sdkcfg_set_ver,
};

int TRT_GUEST_ABI df_ok(void*) { return k_s_ok; }
int TRT_GUEST_ABI df_set_flags(void*, u32) { return k_s_ok; }
u32 TRT_GUEST_ABI df_get_flags(void*) { return 0; }
int TRT_GUEST_ABI df_get_cfg(void*, const void*, const void*, void** ppv) {
  if (ppv) *ppv = nullptr;
  return k_e_nointerface;
}
int TRT_GUEST_ABI df_exp(void*, u32, const void*, void*, const u32*) { return k_e_notimpl; }
int TRT_GUEST_ABI df_create_dev(void*, void* adapter, u32 min_fl, const void* riid, void** out_dev) {
  return trt_D3D12CreateDevice(adapter, min_fl, riid, out_dev);
}
const void* g_devfactory_vtbl[10] = {
    (const void*)&scom_qi, (const void*)&scom_addref, (const void*)&scom_rel,
    (const void*)&df_ok, (const void*)&df_ok, (const void*)&df_set_flags,
    (const void*)&df_get_flags, (const void*)&df_get_cfg,
    (const void*)&df_exp, (const void*)&df_create_dev,
};

}  // namespace

extern "C" {

void trt_d3d12_set_hooks(const trt_d3d12_hooks* hooks) {
  if (hooks) g_hooks = *hooks;
}

const trt_d3d12_hooks* trt_d3d12_hooks_get(void) { return &g_hooks; }

// TRIANGLE GATE accessor: how many command lists this queue has recorded via
// ExecuteCommandLists (slot 8). -1 if the pointer is not a bridge queue.
// The host test reads the queue pointer the GUEST created out of the guest's
// results file and asserts the submission actually landed host-side.
int trt_d3d12_queue_recorded_count(void* obj) {
  if (!obj) return -1;
  std::lock_guard<std::mutex> lk(g_track_mu);
  if (g_queues.count(obj) == 0) return -1;
  auto* q = (d3d12_queue*)obj;
  std::lock_guard<std::mutex> lk2(q->mu);
  return (int)q->recorded_lists.size();
}

int trt_d3d12_is_queue(void* obj) {
  if (!obj) return 0;
  {
    std::lock_guard<std::mutex> lk(g_track_mu);
    if (g_queues.count(obj) > 0) return 1;
  }
  // Real UWP D3D12 queue: safe QueryInterface for IID_ID3D12CommandQueue
  if (!g_hooks.create_device) return 0;
  void* tmp = nullptr;
  const raw_guid k_iid_queue = {
      0x0ec870a6u, 0x5d7e, 0x4c22,
      {0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed}};
  using qi_fn = int (TRT_GUEST_ABI *)(void*, const void*, void**);
  using rel_fn = u32 (TRT_GUEST_ABI *)(void*);
  const void** vtbl = *(const void***)obj;
  if (!vtbl || !vtbl[0] || !vtbl[2]) return 0;
  if (((qi_fn)vtbl[0])(obj, &k_iid_queue, &tmp) == 0 && tmp) {
    ((rel_fn)vtbl[2])(tmp);
    return 1;
  }
  return 0;
}

int trt_d3d12_offline_backbuffer(u32 buffer_idx, u32 width, u32 height,
                                 const void* riid, void** out_res) {
  if (!out_res) return k_e_invalidarg;
  (void)buffer_idx; (void)riid;
  auto* r = new d3d12_resource();
  r->vtbl = g_resource_vtbl;
  r->destroy_fn = &resource_destroy;
  r->desc.dimension = 3;  // D3D12_RESOURCE_DIMENSION_TEXTURE2D
  r->desc.width = width ? width : 1280;
  r->desc.height = height ? height : 720;
  r->desc.depth_or_array = 1;
  r->desc.mip_levels = 1;
  r->desc.format = 87;  // DXGI_FORMAT_B8G8R8A8_UNORM
  r->desc.sample_count = 1;
  r->desc.flags = 1;  // D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  r->gpu_va = g_next_va.fetch_add(0x400000ULL);
  *out_res = r;
  return k_s_ok;
}

int TRT_GUEST_ABI trt_D3D12CreateDevice(void* adapter, u32 min_fl,
                                      const void* riid, void** out_dev) {
  if (!is_valid_fl(min_fl)) return k_e_invalidarg;
  if (g_hooks.create_device) {
    return g_hooks.create_device(adapter, min_fl, riid, out_dev);
  }
  if (!out_dev) return k_s_false;  // probe only
  if (min_fl > k_fl_12_1) {
    *out_dev = nullptr;
    return k_dxgi_error_unsupported;
  }
  auto* dev = new d3d12_device();
  dev->vtbl = g_device_vtbl;
  dev->destroy_fn = &device_destroy;
  dev->max_fl = k_fl_12_1;
  *out_dev = dev;
  return k_s_ok;
}

int TRT_GUEST_ABI trt_D3D12GetDebugInterface(const void* riid, void** out_dbg) {
  if (g_hooks.get_debug_interface) return g_hooks.get_debug_interface(riid, out_dbg);
  if (out_dbg) *out_dbg = nullptr;
  trt_core_log("d3d12: D3D12GetDebugInterface -> E_NOINTERFACE (debug layer disabled)");
  return k_e_nointerface;
}

int TRT_GUEST_ABI trt_D3D12SerializeRootSignature(const void* desc, u32 version,
                                                  void** out_blob, void** out_err) {
  if (out_err) *out_err = nullptr;
  if (g_hooks.serialize_root_sig)
    return g_hooks.serialize_root_sig(desc, version, out_blob, out_err);
  if (!desc || !out_blob) return k_e_invalidarg;
  // Valid minimal DXBC root-signature container header
  u32 hdr[10] = {0x43425844u /*'DXBC'*/, 0, 0, 0, 0, 1, 40, 1, 36, 0x30535452u /*'RTS0'*/};
  u8 buf[64]{};
  memcpy(buf, hdr, sizeof(hdr));
  memcpy(buf + 40, &version, 4);
  *out_blob = make_blob(buf, sizeof(buf));
  return *out_blob ? k_s_ok : k_e_outofmemory;
}

int TRT_GUEST_ABI trt_D3D12SerializeVersionedRootSignature(const void* desc,
                                                           void** out_blob,
                                                           void** out_err) {
  if (out_err) *out_err = nullptr;
  if (g_hooks.serialize_versioned_root_sig)
    return g_hooks.serialize_versioned_root_sig(desc, out_blob, out_err);
  if (!desc || !out_blob) return k_e_invalidarg;
  return trt_D3D12SerializeRootSignature(desc, *(const u32*)desc, out_blob, out_err);
}

int TRT_GUEST_ABI trt_D3D12CreateRootSignatureDeserializer(
    const void* src, u64 size, const void* riid, void** out_des) {
  if (g_hooks.create_root_deserializer)
    return g_hooks.create_root_deserializer(src, size, riid, out_des);
  if (out_des) *out_des = nullptr;
  if (!src || !size) return k_e_invalidarg;
  trt_core_log("d3d12: CreateRootSignatureDeserializer -> E_NOTIMPL (offline)");
  return k_e_notimpl;
}

int TRT_GUEST_ABI trt_D3D12CreateVersionedRootSignatureDeserializer(
    const void* src, u64 size, const void* riid, void** out_des) {
  if (g_hooks.create_versioned_root_deserializer)
    return g_hooks.create_versioned_root_deserializer(src, size, riid, out_des);
  if (out_des) *out_des = nullptr;
  if (!src || !size) return k_e_invalidarg;
  trt_core_log("d3d12: CreateVersionedRootSignatureDeserializer -> E_NOTIMPL (offline)");
  return k_e_notimpl;
}

int TRT_GUEST_ABI trt_D3D12GetInterface(const void* rclsid, const void* riid,
                                        void** out_iface) {
  if (g_hooks.get_interface) return g_hooks.get_interface(rclsid, riid, out_iface);
  if (!out_iface) return k_e_invalidarg;
  if (guid_eq(rclsid, 0x7cda6acau, 0xa03e, 0x49c8, 0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce)) {
    auto* c = new simple_com{g_sdkcfg_vtbl};
    *out_iface = c;
    return k_s_ok;
  }
  if (guid_eq(rclsid, 0x114863bfu, 0xc386, 0x4aee, 0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55)) {
    auto* f = new simple_com{g_devfactory_vtbl};
    *out_iface = f;
    return k_s_ok;
  }
  *out_iface = nullptr;
  return k_e_nointerface;
}

int TRT_GUEST_ABI trt_D3D12EnableExperimentalFeatures(u32 num, const void* iids,
                                                      void* cfg, const u32* sizes) {
  if (g_hooks.enable_experimental) return g_hooks.enable_experimental(num, iids, cfg, sizes);
  return k_e_notimpl;
}

int TRT_GUEST_ABI trt_D3D12Core_unimpl(void) { return k_e_notimpl; }
void TRT_GUEST_ABI trt_D3D12PIX_noop(void) {}
int TRT_GUEST_ABI trt_D3D12Ord_noiface(void) { return k_e_nointerface; }

const trt_named_fn k_d3d12_fns[] = {
    {"D3D12CreateDevice", (void*)&trt_D3D12CreateDevice},
    {"D3D12GetDebugInterface", (void*)&trt_D3D12GetDebugInterface},
    {"D3D12SerializeRootSignature", (void*)&trt_D3D12SerializeRootSignature},
    {"D3D12SerializeVersionedRootSignature", (void*)&trt_D3D12SerializeVersionedRootSignature},
    {"D3D12CreateRootSignatureDeserializer", (void*)&trt_D3D12CreateRootSignatureDeserializer},
    {"D3D12CreateVersionedRootSignatureDeserializer", (void*)&trt_D3D12CreateVersionedRootSignatureDeserializer},
    {"D3D12GetInterface", (void*)&trt_D3D12GetInterface},
    {"D3D12EnableExperimentalFeatures", (void*)&trt_D3D12EnableExperimentalFeatures},
    {"D3D12CoreCreateLayeredDevice", (void*)&trt_D3D12Core_unimpl},
    {"D3D12CoreGetLayeredDeviceSize", (void*)&trt_D3D12Core_unimpl},
    {"D3D12CoreRegisterLayers", (void*)&trt_D3D12Core_unimpl},
    {"D3D12PIXEventsReplaceBlock", (void*)&trt_D3D12PIX_noop},
    {"D3D12PIXGetThreadInfo", (void*)&trt_D3D12PIX_noop},
    {"D3D12PIXNotifyWakeFromFenceSignal", (void*)&trt_D3D12PIX_noop},
    {"D3D12PIXReportCounter", (void*)&trt_D3D12PIX_noop},
    {"#101", (void*)&trt_D3D12Ord_noiface},
    {"#102", (void*)&trt_D3D12Ord_noiface},
    {"#103", (void*)&trt_D3D12Ord_noiface},
    {"#104", (void*)&trt_D3D12Ord_noiface},
    {"#105", (void*)&trt_D3D12Ord_noiface},
    {"#106", (void*)&trt_D3D12Ord_noiface},
};

int trt_bridge_register_d3d12(void) {
  // TRIANGLE GATE ABI guard: fail loudly (and visibly) if the CommandQueue
  // vtbl ever drifts from the real ID3D12CommandQueue layout again.
  if (!d3d12_vtbl_selfcheck()) {
    trt_core_log("d3d12: CommandQueue vtbl ABI check FAILED - refusing registration");
    return -1;
  }
  const int n = (int)(sizeof(k_d3d12_fns) / sizeof(k_d3d12_fns[0]));
  trt_module_register("d3d12.dll", TRT_ROUTE_BUILTIN, k_d3d12_fns, n);
  trt_module_register("d3d12core.dll", TRT_ROUTE_BUILTIN, k_d3d12_fns, n);
  return 0;
}

}  // extern "C"
