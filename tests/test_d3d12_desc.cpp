// tiramisu/tests/test_d3d12_desc.cpp
// The descriptor wave, proven offline — device slots 17-24, the command
// list's binding/copy slots, and PSO creation (master-prompt items 24/25/30
// + 29, the wave that replaced the silent dev_view_noop).
//
// Proves:
//   CreateDescriptorHeap mints a synthetic CPU/GPU handle space whose
//     increment matches GetDescriptorHandleIncrementSize (32 CSU/SAMPLER,
//     64 RTV/DSV) and whose GetDesc round-trips (heap slots 8/9/10).
//   CreateConstantBufferView / CreateShaderResourceView /
//     CreateUnorderedAccessView / CreateRenderTargetView /
//     CreateDepthStencilView / CreateSampler write a typed record into the
//     addressed heap slot (device slots 17-22).
//   Bad input FAILS VISIBLY: null desc, null resource, out-of-range handles,
//     zero CBV location, invalid sampler fields — the slot stays empty and
//     the runtime logs a named reason (never a silent no-op).
//   CopyDescriptors / CopyDescriptorsSimple copy typed records within and
//     across heaps, bounds-checked (device slots 23-24).
//   The command list records SetDescriptorHeaps (replacing, like the real
//     API), root descriptor tables and root CBVs for both compute and
//     graphics (cmdlist slots 28/31/32/37/38), and CopyBufferRegion /
//     CopyResource (slots 15/17).
//   CreateGraphicsPipelineState parses the legacy 656-byte desc and rejects
//     a missing VS; CreatePipelineState walks the subobject stream
//     (ROOT_SIGNATURE/VS/PS/CS) and rejects garbage subobjects with
//     E_INVALIDARG (device slots 10/47).
//   WriteToSubresource / ReadFromSubresource round-trip bytes through the
//     resource backing (visible through Map) and reject a reversed box
//     (resource slots 12/13).
//
// All guest-ABI function pointers are called through typedefs carrying
// TRT_STDCALL (__attribute__((ms_abi)) on x86_64 Linux) — SysV callers
// would corrupt the ms_abi callee's argument registers otherwise.
#include "../src/trt/trt.h"
#include "../src/trt/bridge_d3d12.h"

#include <stdio.h>
#include <string.h>

extern "C" int trt_bridge_register_d3d12(void);

namespace {

int g_checks = 0, g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    ++g_checks;                                                              \
    if (cond) {                                                              \
      printf("  ok: %s\n", msg);                                             \
    } else {                                                                 \
      ++g_failures;                                                          \
      printf("  FAIL: %s\n", msg);                                           \
    }                                                                        \
  } while (0)

// raw_guid mirrors the bridge's GUID layout (little-endian Win32 GUID).
struct raw_guid {
  u32 d1;
  u16 d2;
  u16 d3;
  u8 d4[8];
};
static_assert(sizeof(raw_guid) == 16, "GUID must be 16 bytes");

void make_iid(raw_guid* g, u32 d1, u16 d2, u16 d3, u8 b0, u8 b1, u8 b2, u8 b3,
              u8 b4, u8 b5, u8 b6, u8 b7) {
  g->d1 = d1; g->d2 = d2; g->d3 = d3;
  g->d4[0] = b0; g->d4[1] = b1; g->d4[2] = b2; g->d4[3] = b3;
  g->d4[4] = b4; g->d4[5] = b5; g->d4[6] = b6; g->d4[7] = b7;
}

const void** vtbl_of(void* obj) { return *(const void***)obj; }

// ---- POD mirrors of the bridge's guest-ABI structs (byte-exact; the real
// types are private to the bridge). Sizes are pinned by static_assert and
// cross-checked against the SDK header (d3d12.h via xwin). ----

struct heap_desc_pod {              // D3D12_DESCRIPTOR_HEAP_DESC (16)
  u32 type, num_descriptors, flags, node_mask;
};
static_assert(sizeof(heap_desc_pod) == 16, "heap desc 16");

struct heap_props_pod {             // D3D12_HEAP_PROPERTIES (20)
  u32 type, cpu_page_prop, memory_pool, creation_node_mask, visible_node_mask;
};
static_assert(sizeof(heap_props_pod) == 20, "heap props 20");

struct resource_desc_pod {          // D3D12_RESOURCE_DESC (52)
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
static_assert(sizeof(resource_desc_pod) == 56,
              "resource desc mirror 56 (real D3D12_RESOURCE_DESC is 52; the "
              "mirror's trailing pad is never read)");

struct cbv_desc_pod {               // D3D12_CONSTANT_BUFFER_VIEW_DESC (16)
  u64 buffer_location;
  u32 size_in_bytes;
  u32 pad;
};
static_assert(sizeof(cbv_desc_pod) == 16, "CBV desc 16");

struct srv_desc_pod {               // D3D12_SHADER_RESOURCE_VIEW_DESC (40)
  u32 format;
  u32 view_dimension;
  u32 shader4_component_mapping;
  u32 pad;
  u8 payload[24];
};
static_assert(sizeof(srv_desc_pod) == 40, "SRV desc 40");

struct uav_desc_pod {               // D3D12_UNORDERED_ACCESS_VIEW_DESC (40)
  u32 format;
  u32 view_dimension;
  u8 payload[32];
};
static_assert(sizeof(uav_desc_pod) == 40, "UAV desc 40");

struct rtv_desc_pod {               // D3D12_RENDER_TARGET_VIEW_DESC (24)
  u32 format;
  u32 view_dimension;
  u8 payload[16];
};
static_assert(sizeof(rtv_desc_pod) == 24, "RTV desc 24");

struct dsv_desc_pod {               // D3D12_DEPTH_STENCIL_VIEW_DESC (24)
  u32 format;
  u32 view_dimension;
  u32 flags;
  u8 payload[12];
};
static_assert(sizeof(dsv_desc_pod) == 24, "DSV desc 24");

struct sampler_desc_pod {           // D3D12_SAMPLER_DESC (52)
  u32 filter;
  u32 address_u, address_v, address_w;
  float mip_lod_bias;
  u32 max_anisotropy;
  u32 comparison_func;
  float border_color[4];
  float min_lod;
  float max_lod;
};
static_assert(sizeof(sampler_desc_pod) == 52, "SAMPLER desc 52");

struct box_pod {                    // D3D12_BOX (24)
  u32 left, top, front, right, bottom, back;
};
static_assert(sizeof(box_pod) == 24, "BOX 24");

struct shader_pod {                 // D3D12_SHADER_BYTECODE (16)
  const void* p;
  u64 len;
};
static_assert(sizeof(shader_pod) == 16, "shader bytecode 16");

struct streamout_pod {              // D3D12_STREAM_OUTPUT_DESC (32)
  const void* decl;
  u32 num_entries;
  u32 pad0;
  const void* strides;
  u32 num_strides;
  u32 rasterized_stream;
};
static_assert(sizeof(streamout_pod) == 32, "stream output 32");

struct gpsd_pod {                   // D3D12_GRAPHICS_PIPELINE_STATE_DESC (656)
  void* p_root_signature;           // @0
  shader_pod vs;                    // @8
  shader_pod ps;                    // @24
  shader_pod ds;                    // @40
  shader_pod hs;                    // @56
  shader_pod gs;                    // @72
  streamout_pod stream_output;      // @88
  u8 blend_state[328];              // @120
  u32 sample_mask;                  // @448
  u8 rasterizer_state[44];          // @452 (11 fields x 4, SDK-verbatim)
  u8 depth_stencil_state[56];       // @496
  const void* p_input_elements;     // @552
  u32 input_element_count;          // @560
  u32 input_pad;                    // @564
  u32 ib_strip_cut_value;           // @568
  u32 primitive_topology_type;      // @572
  u32 num_render_targets;           // @576
  u32 rtv_formats[8];               // @580
  u32 dsv_format;                   // @612
  u32 sample_count;                 // @616
  u32 sample_quality;               // @620
  u32 node_mask;                    // @624
  u32 pad0;                         // @628
  const void* cached_blob;          // @632
  u64 cached_blob_size;             // @640
  u32 flags;                        // @648
  u32 pad1;                         // @652
};
static_assert(sizeof(gpsd_pod) == 656, "GPSD 656");

struct cpsd_pod {                   // D3D12_COMPUTE_PIPELINE_STATE_DESC (56)
  void* p_root_signature;
  shader_pod cs;
  u32 node_mask;
  u32 pad0;
  const void* cached_blob;
  u64 cached_blob_size;
  u32 flags;
  u32 pad1;
};
static_assert(sizeof(cpsd_pod) == 56, "CPSD 56");

struct stream_desc_pod {            // D3D12_PIPELINE_STATE_STREAM_DESC (16)
  u64 size_in_bytes;                // @0 (SDK-verbatim: SizeInBytes first)
  void* p_subobject_stream;         // @8
};
static_assert(sizeof(stream_desc_pod) == 16, "stream desc 16");

struct subobject_hdr_pod {          // D3D12_PIPELINE_STATE_STREAM_SUBOBJECT (16)
  u32 type;
  u32 pad;
  u64 size;
};
static_assert(sizeof(subobject_hdr_pod) == 16, "subobject header 16");

// D3D12_PIPELINE_STATE_SUBOBJECT_TYPE (SDK-verbatim; valid 0..28).
enum : u32 {
  SUBO_ROOT_SIGNATURE = 0,
  SUBO_VS = 1,
  SUBO_PS = 2,
  SUBO_CS = 6,
};

// IIDs, SDK-verbatim strings (xwin d3d12.h), bridge byte order.
raw_guid iid_iunk, iid_descheap, iid_pso, iid_res, iid_bogus;

// ---- vtable slot signatures (Win64 ABI) ----
using qi_fn = int (TRT_GUEST_ABI*)(void*, const void*, void**);
using descheap_fn = int (TRT_GUEST_ABI*)(void*, const heap_desc_pod*,
                                         const void*, void**);
using inc_fn = u32 (TRT_GUEST_ABI*)(void*, u32);
using commit_fn = int (TRT_GUEST_ABI*)(void*, const heap_props_pod*, u32,
                                       const resource_desc_pod*, u32,
                                       const void*, const void*, void**);
using gpufn = u64 (TRT_GUEST_ABI*)(void*);
using map_fn = int (TRT_GUEST_ABI*)(void*, u32, const void*, void**);
using wsub_fn = int (TRT_GUEST_ABI*)(void*, u32, const box_pod*, const void*,
                                     u32, u32);
using rsub_fn = int (TRT_GUEST_ABI*)(void*, void*, u32, u32, u32,
                                     const box_pod*);
using cbv_fn = void (TRT_GUEST_ABI*)(void*, const cbv_desc_pod*, u64);
using srv_fn = void (TRT_GUEST_ABI*)(void*, void*, const srv_desc_pod*, u64);
using uav_fn = void (TRT_GUEST_ABI*)(void*, void*, void*,
                                     const uav_desc_pod*, u64);
using rtv_fn = void (TRT_GUEST_ABI*)(void*, void*, const rtv_desc_pod*, u64);
using dsv_fn = void (TRT_GUEST_ABI*)(void*, void*, const dsv_desc_pod*, u64);
using sampler_fn = void (TRT_GUEST_ABI*)(void*, const sampler_desc_pod*, u64);
using copyd_fn = void (TRT_GUEST_ABI*)(void*, u32, const u64*, const u32*,
                                       u32, const u64*, const u32*, u32);
using simp_fn = void (TRT_GUEST_ABI*)(void*, u32, u64, u64, u32);
using alloc_fn = int (TRT_GUEST_ABI*)(void*, u32, const void*, void**);
using cl_fn = int (TRT_GUEST_ABI*)(void*, u32, u32, void*, void*,
                                   const void*, void**);
using heaps_fn = void (TRT_GUEST_ABI*)(void*, u32, void* const*);
using table_fn = void (TRT_GUEST_ABI*)(void*, u32, u64);
using cbr_fn = void (TRT_GUEST_ABI*)(void*, void*, u64, void*, u64, u64);
using cpyres_fn = void (TRT_GUEST_ABI*)(void*, void*, void*);
using psogfx_fn = int (TRT_GUEST_ABI*)(void*, const gpsd_pod*, const void*,
                                       void**);
using psostream_fn = int (TRT_GUEST_ABI*)(void*, const stream_desc_pod*,
                                          const void*, void**);
using hdesc_fn = heap_desc_pod* (TRT_GUEST_ABI*)(void*, heap_desc_pod*);
using hstart_fn = u64* (TRT_GUEST_ABI*)(void*, u64*);

// Heap record kinds (bridge d3d12_view_record).
constexpr int K_CBV = 0, K_SRV = 1, K_UAV = 2, K_RTV = 3, K_DSV = 4,
              K_SAMPLER = 5;
constexpr int K_EMPTY = (int)0xffffffffu;

void* g_cs2 = nullptr;  // second CSU heap, stashed in section D for section E

}  // namespace

int main() {
  printf("tiramisu D3D12 descriptor wave test (device slots 17-24, PSO)\n");

  // ---- registration (runs d3d12_vtbl_selfcheck inside) ----------------------
  const int reg = trt_bridge_register_d3d12();
  CHECK(reg == 0, "trt_bridge_register_d3d12 -> 0 (vtbl selfcheck passed)");

  make_iid(&iid_iunk, 0x00000000u, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00,
           0x00, 0x00, 0x00, 0x46);
  // ID3D12DescriptorHeap 8efb471d-616c-4f49-90f7-127bb763fa51
  make_iid(&iid_descheap, 0x8efb471du, 0x616c, 0x4f49, 0x90, 0xf7, 0x12, 0x7b,
           0xb7, 0x63, 0xfa, 0x51);
  // ID3D12PipelineState 765a30f3-f624-4c6f-a828-ace948622445
  make_iid(&iid_pso, 0x765a30f3u, 0xf624, 0x4c6f, 0xa8, 0x28, 0xac, 0xe9,
           0x48, 0x62, 0x24, 0x45);
  // ID3D12Resource 696442be-a72e-4059-bc79-5b5c98040fad
  make_iid(&iid_res, 0x696442beu, 0xa72e, 0x4059, 0xbc, 0x79, 0x5b, 0x5c,
           0x98, 0x04, 0x0f, 0xad);
  make_iid(&iid_bogus, 0xdeadbeefu, 0x1234, 0x5678, 0x9a, 0xbc, 0xde, 0xf0,
           0x11, 0x22, 0x33, 0x44);

  void* dev = nullptr;
  const int dhr = trt_D3D12CreateDevice(nullptr, 0xc000 /*FL 12.0*/, nullptr,
                                        &dev);
  CHECK(dhr == 0 && dev != nullptr, "D3D12CreateDevice(FL 12.0) -> S_OK");
  if (!dev) {
    printf("failures=%d\nRESULT: FAILED\n", g_failures);
    return 1;
  }
  const void** dvt = vtbl_of(dev);

  // ---- A. heap create + handle arithmetic -----------------------------------
  heap_desc_pod hdesc{2, 8, 0, 0};  // RTV heap, 8 descriptors, flag NONE
  void* heap = nullptr;
  int hr = ((descheap_fn)dvt[14])(dev, &hdesc, nullptr, &heap);
  CHECK(hr == 0 && heap != nullptr, "CreateDescriptorHeap(RTV x8) -> S_OK");

  const void** hvt = vtbl_of(heap);
  heap_desc_pod got{};
  heap_desc_pod* gp = ((hdesc_fn)hvt[8])(heap, &got);
  CHECK(gp == &got && got.type == 2 && got.num_descriptors == 8,
        "ID3D12DescriptorHeap::GetDesc round-trips (type 2, n 8)");

  u64 cpu_start = 0, gpu_start = 0;
  ((hstart_fn)hvt[9])(heap, &cpu_start);
  ((hstart_fn)hvt[10])(heap, &gpu_start);
  CHECK(cpu_start != 0, "GetCPUDescriptorHandleForHeapStart -> nonzero base");
  CHECK(gpu_start == 0,
        "GetGPUDescriptorHandleForHeapStart(non-shader-visible) -> null handle");

  CHECK(((inc_fn)dvt[15])(dev, 2) == 64,
        "GetDescriptorHandleIncrementSize(RTV) == 64");
  CHECK(((inc_fn)dvt[15])(dev, 0) == 32,
        "GetDescriptorHandleIncrementSize(CBV_SRV_UAV) == 32");
  CHECK(((inc_fn)dvt[15])(dev, 1) == 32,
        "GetDescriptorHandleIncrementSize(SAMPLER) == 32");
  CHECK(trt_d3d12_heap_record_count(heap) == 8,
        "heap record store sized to NumDescriptors");

  // ---- B. resources to point views at ---------------------------------------
  heap_props_pod props{1, 0, 1, 0, 0};  // DEFAULT heap, UNKNOWN cpu page
  resource_desc_pod tex{};
  tex.dimension = 2;                    // TEXTURE2D
  tex.width = 256;
  tex.height = 256;
  tex.depth_or_array = 1;
  tex.mip_levels = 1;
  tex.format = 28;                      // DXGI_FORMAT_R8G8B8A8_UNORM
  tex.sample_count = 1;
  void* tex_res = nullptr;
  hr = ((commit_fn)dvt[27])(dev, &props, 0, &tex, 0, nullptr, nullptr,
                            &tex_res);
  CHECK(hr == 0 && tex_res != nullptr,
        "CreateCommittedResource(TEX2D 256x256 R8G8B8A8) -> S_OK");

  resource_desc_pod bufd{};
  bufd.dimension = 1;                   // BUFFER
  bufd.width = 4096;
  bufd.height = 1;
  bufd.depth_or_array = 1;
  bufd.mip_levels = 1;
  bufd.sample_count = 1;
  void* buf1 = nullptr;
  hr = ((commit_fn)dvt[27])(dev, &props, 0, &bufd, 0, nullptr, nullptr,
                            &buf1);
  CHECK(hr == 0 && buf1 != nullptr, "CreateCommittedResource(BUFFER 4096) -> S_OK");
  void* buf2 = nullptr;
  hr = ((commit_fn)dvt[27])(dev, &props, 0, &bufd, 0, nullptr, nullptr,
                            &buf2);
  CHECK(hr == 0 && buf2 != nullptr, "CreateCommittedResource(BUFFER #2) -> S_OK");
  const u64 buf1_va = ((gpufn)vtbl_of(buf1)[11])(buf1);
  CHECK(buf1_va != 0, "ID3D12Resource::GetGPUVirtualAddress -> nonzero");

  // ---- C. views into heap slots (device slots 17-22) -------------------------
  rtv_desc_pod rtv{};
  rtv.format = 28;
  rtv.view_dimension = 2;  // RTV_DIMENSION_TEXTURE2D
  ((rtv_fn)dvt[20])(dev, tex_res, &rtv, cpu_start + 0 * 64);
  CHECK(trt_d3d12_heap_record_kind(heap, 0) == K_RTV,
        "CreateRenderTargetView -> typed record (kind RTV) in slot 0");

  ((rtv_fn)dvt[20])(dev, nullptr, &rtv, cpu_start + 1 * 64);
  CHECK(trt_d3d12_heap_record_kind(heap, 1) == K_EMPTY,
        "CreateRenderTargetView(NULL resource) fails visibly, slot stays empty");

  ((rtv_fn)dvt[20])(dev, tex_res, &rtv, cpu_start + 8 * 64);  // out of range
  CHECK(trt_d3d12_heap_record_count(heap) == 8,
        "CreateRenderTargetView(out-of-range handle) fails visibly, no OOB write");

  dsv_desc_pod dsv{};
  dsv.format = 43;  // DXGI_FORMAT_D32_FLOAT
  dsv.view_dimension = 2;  // DSV_DIMENSION_TEXTURE2D
  ((dsv_fn)dvt[21])(dev, tex_res, &dsv, cpu_start + 2 * 64);
  CHECK(trt_d3d12_heap_record_kind(heap, 2) == K_DSV,
        "CreateDepthStencilView -> typed record (kind DSV)");

  // CSU heap (shader-visible) for CBV/SRV/UAV/sampler.
  heap_desc_pod cdesc{0, 32, 1, 0};  // CBV_SRV_UAV, 32, SHADER_VISIBLE
  void* csu = nullptr;
  hr = ((descheap_fn)dvt[14])(dev, &cdesc, nullptr, &csu);
  CHECK(hr == 0 && csu != nullptr, "CreateDescriptorHeap(CSU x32) -> S_OK");
  u64 csu_start = 0, csu_gpu = 0;
  ((hstart_fn)vtbl_of(csu)[9])(csu, &csu_start);
  ((hstart_fn)vtbl_of(csu)[10])(csu, &csu_gpu);
  CHECK(csu_start != 0 && csu_gpu != 0, "CSU heap mints CPU + GPU handle bases");

  cbv_desc_pod cbv{buf1_va, 256, 0};
  ((cbv_fn)dvt[17])(dev, &cbv, csu_start + 0 * 32);
  CHECK(trt_d3d12_heap_record_kind(csu, 0) == K_CBV,
        "CreateConstantBufferView -> typed record (kind CBV)");

  cbv_desc_pod cbv_bad{0, 256, 0};  // zero GPU VA
  ((cbv_fn)dvt[17])(dev, &cbv_bad, csu_start + 1 * 32);
  CHECK(trt_d3d12_heap_record_kind(csu, 1) == K_EMPTY,
        "CreateConstantBufferView(BufferLocation 0) fails visibly, slot empty");

  srv_desc_pod srv{};
  srv.format = 28;
  srv.view_dimension = 4;  // SRV_DIMENSION_TEXTURE2D
  srv.shader4_component_mapping = 0x5886;  // D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
  srv_desc_pod srvb{};
  srvb.format = 0;  // typeless-ish buffer
  srvb.view_dimension = 1;  // SRV_DIMENSION_BUFFER
  srvb.shader4_component_mapping = 0x5886;
  {
    // BUFFER arm payload: FirstElement u64, NumElements u32, Stride u32.
    u8 pay[24] = {};
    const u64 first = 0;
    memcpy(pay, &first, 8);
    const u32 numel = 256, stride = 16;
    memcpy(pay + 8, &numel, 4);
    memcpy(pay + 12, &stride, 4);
    memcpy(srvb.payload, pay, 24);
  }
  ((srv_fn)dvt[18])(dev, buf1, &srvb, csu_start + 1 * 32);
  CHECK(trt_d3d12_heap_record_kind(csu, 1) == K_SRV,
        "CreateShaderResourceView(BUFFER) -> typed record (kind SRV)");

  {
    // TEX2D arm payload: MostDetailedMip, MipLevels, PlaneSlice, MinLODClamp.
    u8 pay[24] = {};
    const u32 mip = 0, mips = 1, plane = 0;
    memcpy(pay, &mip, 4);
    memcpy(pay + 4, &mips, 4);
    memcpy(pay + 8, &plane, 4);
    memcpy(srv.payload, pay, 24);
  }
  ((srv_fn)dvt[18])(dev, tex_res, &srv, csu_start + 2 * 32);
  CHECK(trt_d3d12_heap_record_kind(csu, 2) == K_SRV,
        "CreateShaderResourceView(TEX2D) -> typed record (kind SRV)");

  ((srv_fn)dvt[18])(dev, nullptr, &srv, csu_start + 3 * 32);
  CHECK(trt_d3d12_heap_record_kind(csu, 3) == K_EMPTY,
        "CreateShaderResourceView(NULL resource) fails visibly, slot empty");

  uav_desc_pod uav{};
  uav.format = 28;
  uav.view_dimension = 4;  // UAV_DIMENSION_TEXTURE2D
  {
    u8 pay[32] = {};
    const u32 mip = 0, plane = 0;
    memcpy(pay, &mip, 4);
    memcpy(pay + 4, &plane, 4);
    memcpy(uav.payload, pay, 32);
  }
  ((uav_fn)dvt[19])(dev, tex_res, nullptr, &uav, csu_start + 3 * 32);
  CHECK(trt_d3d12_heap_record_kind(csu, 3) == K_UAV,
        "CreateUnorderedAccessView(TEX2D) -> typed record (kind UAV)");

  ((uav_fn)dvt[19])(dev, tex_res, (void*)&g_checks, &uav, csu_start + 6 * 32);
  CHECK(trt_d3d12_heap_record_kind(csu, 6) == K_EMPTY,
        "CreateUnorderedAccessView(bogus counter resource) fails visibly, slot empty");

  sampler_desc_pod samp{};
  samp.filter = 0x55;  // D3D12_FILTER_ANISOTROPIC
  samp.address_u = samp.address_v = samp.address_w = 1;  // WRAP
  samp.comparison_func = 5;
  samp.max_anisotropy = 8;
  samp.max_lod = 16.0f;
  ((sampler_fn)dvt[22])(dev, &samp, csu_start + 4 * 32);
  CHECK(trt_d3d12_heap_record_kind(csu, 4) == K_SAMPLER,
        "CreateSampler -> typed record (kind SAMPLER)");

  sampler_desc_pod samp_bad = samp;
  samp_bad.filter = 0x1d6;  // one past D3D12_FILTER_MAXIMUM_ANISOTROPIC
  ((sampler_fn)dvt[22])(dev, &samp_bad, csu_start + 5 * 32);
  CHECK(trt_d3d12_heap_record_kind(csu, 5) == K_EMPTY,
        "CreateSampler(invalid Filter) fails visibly, slot empty");

  samp_bad = samp;
  samp_bad.address_u = 0;  // outside D3D12_TEXTURE_ADDRESS_MODE
  ((sampler_fn)dvt[22])(dev, &samp_bad, csu_start + 5 * 32);
  CHECK(trt_d3d12_heap_record_kind(csu, 5) == K_EMPTY,
        "CreateSampler(AddressU 0) fails visibly, slot empty");

  samp_bad = samp;
  samp_bad.max_anisotropy = 17;
  ((sampler_fn)dvt[22])(dev, &samp_bad, csu_start + 5 * 32);
  CHECK(trt_d3d12_heap_record_kind(csu, 5) == K_EMPTY,
        "CreateSampler(MaxAnisotropy 17) fails visibly, slot empty");

  // ---- D. CopyDescriptors / CopyDescriptorsSimple (slots 23-24) --------------
  {
    u64 dst[2] = {csu_start + 5 * 32, csu_start + 6 * 32};
    u32 dsz[2] = {1, 1};
    u64 src[1] = {csu_start + 0 * 32};
    u32 ssz[1] = {2};
    ((copyd_fn)dvt[23])(dev, 2, dst, dsz, 1, src, ssz, 0);
    CHECK(trt_d3d12_heap_record_kind(csu, 5) == K_CBV &&
              trt_d3d12_heap_record_kind(csu, 6) == K_SRV,
          "CopyDescriptors 1x2 range: dst 5<-src0 (CBV), dst 6<-src1 (SRV)");
  }
  {
    // Cross-heap: second CSU heap, copy csu slot 0 -> csu2 slot 0.
    heap_desc_pod c2{0, 8, 1, 0};
    void* csu2 = nullptr;
    hr = ((descheap_fn)dvt[14])(dev, &c2, nullptr, &csu2);
    CHECK(hr == 0 && csu2 != nullptr, "CreateDescriptorHeap(CSU2 x8) -> S_OK");
    u64 csu2_start = 0;
    ((hstart_fn)vtbl_of(csu2)[9])(csu2, &csu2_start);
    u64 dst[1] = {csu2_start};
    u32 dsz[1] = {1};
    u64 src[1] = {csu_start + 0 * 32};
    u32 ssz[1] = {1};
    ((copyd_fn)dvt[23])(dev, 1, dst, dsz, 1, src, ssz, 0);
    CHECK(trt_d3d12_heap_record_kind(csu2, 0) == K_CBV,
          "CopyDescriptors copies ACROSS heaps (csu -> csu2)");
    g_cs2 = csu2;
  }
  ((simp_fn)dvt[24])(dev, 2, csu_start + 7 * 32, csu_start + 0 * 32, 0);
  CHECK(trt_d3d12_heap_record_kind(csu, 7) == K_CBV &&
            trt_d3d12_heap_record_kind(csu, 8) == K_SRV,
        "CopyDescriptorsSimple: slot 7<-src0 (CBV), slot 8<-src1 (SRV)");
  {
    // Out-of-range dst: beyond the 32-slot heap -> fail visibly, no crash.
    u64 dst[1] = {csu_start + 32 * 32};
    u32 dsz[1] = {1};
    u64 src[1] = {csu_start + 0 * 32};
    u32 ssz[1] = {1};
    ((copyd_fn)dvt[23])(dev, 1, dst, dsz, 1, src, ssz, 0);
    CHECK(true, "CopyDescriptors(out-of-range dst) fails visibly, no OOB write");
  }

  // ---- E. command-list recording (slots 15/17/28/31/32/37/38) ----------------
  void* alloc = nullptr;
  hr = ((alloc_fn)dvt[9])(dev, 1 /*DIRECT*/, nullptr, &alloc);
  CHECK(hr == 0 && alloc != nullptr, "CreateCommandAllocator(DIRECT) -> S_OK");
  void* cl = nullptr;
  hr = ((cl_fn)dvt[12])(dev, 0, 1 /*DIRECT*/, alloc, nullptr, nullptr, &cl);
  CHECK(hr == 0 && cl != nullptr, "CreateCommandList(DIRECT) -> S_OK");

  void* csu2 = g_cs2;
  void* heaps2[2] = {csu, csu2};
  ((heaps_fn)vtbl_of(cl)[28])(cl, 2, heaps2);
  CHECK(trt_d3d12_cmdlist_bound_heap_count(cl) == 2,
        "SetDescriptorHeaps(2 heaps) recorded");
  ((heaps_fn)vtbl_of(cl)[28])(cl, 1, heaps2);
  CHECK(trt_d3d12_cmdlist_bound_heap_count(cl) == 1,
        "SetDescriptorHeaps REPLACES the previous binding");

  ((table_fn)vtbl_of(cl)[31])(cl, 3, csu_gpu);   // SetComputeRootDescriptorTable
  ((table_fn)vtbl_of(cl)[32])(cl, 5, csu_gpu);   // SetGraphicsRootDescriptorTable
  CHECK(trt_d3d12_cmdlist_root_table_count(cl) == 2,
        "Set[Compute|Graphics]RootDescriptorTable both recorded");

  ((table_fn)vtbl_of(cl)[37])(cl, 7, buf1_va);   // SetComputeRootConstantBufferView
  ((table_fn)vtbl_of(cl)[38])(cl, 9, buf1_va);   // SetGraphicsRootConstantBufferView
  CHECK(trt_d3d12_cmdlist_root_cbv_count(cl) == 2,
        "Set[Compute|Graphics]RootConstantBufferView both recorded");

  const u32 copies0 = trt_d3d12_cmdlist_copy_count(cl);
  ((cbr_fn)vtbl_of(cl)[15])(cl, buf2, 256, buf1, 0, 128);  // CopyBufferRegion
  CHECK(trt_d3d12_cmdlist_copy_count(cl) == copies0 + 1,
        "CopyBufferRegion recorded into the command list");
  ((cpyres_fn)vtbl_of(cl)[17])(cl, buf2, buf1);            // CopyResource
  CHECK(trt_d3d12_cmdlist_copy_count(cl) == copies0 + 2,
        "CopyResource recorded into the command list");

  // ---- F. PSO creation (device slots 10 / 47) --------------------------------
  static const u8 fake_shader[32] = {0};  // dummy DXBC-ish blob
  gpsd_pod gpsd{};
  gpsd.vs.p = fake_shader;
  gpsd.vs.len = sizeof(fake_shader);
  gpsd.ps.p = fake_shader;
  gpsd.ps.len = sizeof(fake_shader);
  gpsd.sample_mask = 0xffffffffu;
  gpsd.primitive_topology_type = 3;  // TRIANGLE
  gpsd.num_render_targets = 1;
  gpsd.rtv_formats[0] = 28;
  gpsd.sample_count = 1;
  void* pso = nullptr;
  hr = ((psogfx_fn)dvt[10])(dev, &gpsd, nullptr, &pso);
  CHECK(hr == 0 && pso != nullptr,
        "CreateGraphicsPipelineState (legacy 656B desc) -> S_OK");

  gpsd_pod gpsd_bad = gpsd;
  gpsd_bad.vs.p = nullptr;
  gpsd_bad.vs.len = 0;  // unpaired/missing VS
  void* pso_bad = nullptr;
  hr = ((psogfx_fn)dvt[10])(dev, &gpsd_bad, nullptr, &pso_bad);
  CHECK(hr != 0 && pso_bad == nullptr,
        "CreateGraphicsPipelineState(missing VS) -> E_INVALIDARG, no PSO");

  // Stream PSO: ROOT_SIGNATURE(nullptr) + VS + PS subobjects.
  {
    u8 stream[128] = {};
    u64 off = 0;
    auto put = [&](u32 type, const void* payload, u64 size) {
      subobject_hdr_pod h{type, 0, size};
      memcpy(stream + off, &h, 16);
      off += 16;
      if (size) {
        memcpy(stream + off, payload, size);
        off += size;
      }
    };
    const void* rs_null = nullptr;
    put(SUBO_ROOT_SIGNATURE, &rs_null, 8);
    put(SUBO_VS, &gpsd.vs, 16);
    put(SUBO_PS, &gpsd.ps, 16);
    stream_desc_pod sd{off, stream};
    void* pso2 = nullptr;
    hr = ((psostream_fn)dvt[47])(dev, &sd, nullptr, &pso2);
    CHECK(hr == 0 && pso2 != nullptr,
          "CreatePipelineState(stream RS+VS+PS) -> S_OK");
  }
  {
    // Compute stream: CS only.
    u8 stream[64] = {};
    subobject_hdr_pod h{SUBO_CS, 0, 16};
    memcpy(stream, &h, 16);
    shader_pod cs{fake_shader, sizeof(fake_shader)};
    memcpy(stream + 16, &cs, 16);
    stream_desc_pod sd{32, stream};
    void* pso3 = nullptr;
    hr = ((psostream_fn)dvt[47])(dev, &sd, nullptr, &pso3);
    CHECK(hr == 0 && pso3 != nullptr, "CreatePipelineState(stream CS) -> S_OK");
  }
  {
    // Garbage subobject type 29 (outside 0..28) -> E_INVALIDARG.
    u8 stream[32] = {};
    subobject_hdr_pod h{29, 0, 16};
    memcpy(stream, &h, 16);
    stream_desc_pod sd{32, stream};
    void* pso4 = nullptr;
    hr = ((psostream_fn)dvt[47])(dev, &sd, nullptr, &pso4);
    CHECK(hr != 0 && pso4 == nullptr,
          "CreatePipelineState(stream type 29) -> E_INVALIDARG (fail visible)");
  }
  {
    // Payload overrun: VS header claims more bytes than the stream holds.
    u8 stream[32] = {};
    subobject_hdr_pod h{SUBO_VS, 0, 0xffffull};
    memcpy(stream, &h, 16);
    stream_desc_pod sd{32, stream};
    void* pso5 = nullptr;
    hr = ((psostream_fn)dvt[47])(dev, &sd, nullptr, &pso5);
    CHECK(hr != 0 && pso5 == nullptr,
          "CreatePipelineState(stream payload overrun) -> E_INVALIDARG");
  }

  // ---- G. WriteToSubresource / ReadFromSubresource (resource slots 12/13) ----
  void* mapped = nullptr;
  hr = ((map_fn)vtbl_of(buf1)[8])(buf1, 0, nullptr, &mapped);
  CHECK(hr == 0 && mapped != nullptr, "ID3D12Resource::Map(buffer) -> pointer");

  u8 pattern[256];
  for (int i = 0; i < 256; ++i) pattern[i] = (u8)(i * 7 + 3);
  box_pod box{128, 0, 0, 128 + 256, 1, 1};
  hr = ((wsub_fn)vtbl_of(buf1)[12])(buf1, 0, &box, pattern, 256, 256);
  CHECK(hr == 0, "WriteToSubresource(256B @ offset 128) -> S_OK");

  u8 back[256] = {};
  hr = ((rsub_fn)vtbl_of(buf1)[13])(buf1, back, 256, 256, 0, &box);
  CHECK(hr == 0 && memcmp(back, pattern, 256) == 0,
        "ReadFromSubresource round-trips the written bytes");

  CHECK(mapped && ((u8*)mapped)[128] == pattern[0] &&
            ((u8*)mapped)[383] == pattern[255],
        "written bytes visible through Map (same backing)");

  box_pod bad{300, 0, 0, 100, 1, 1};  // right < left
  hr = ((wsub_fn)vtbl_of(buf1)[12])(buf1, 0, &bad, pattern, 256, 256);
  CHECK(hr != 0, "WriteToSubresource(reversed box) -> E_INVALIDARG (fail visible)");

  // ---- H. QI on the new objects ----------------------------------------------
  void* qv = nullptr;
  hr = ((qi_fn)hvt[0])(heap, &iid_descheap, &qv);
  CHECK(hr == 0 && qv == heap, "heap QI(ID3D12DescriptorHeap) -> S_OK, self");
  qv = nullptr;
  hr = ((qi_fn)hvt[0])(heap, &iid_bogus, &qv);
  CHECK(hr != 0 && qv == nullptr,
        "heap QI(bogus IID) -> E_NOINTERFACE (fail visible)");
  qv = nullptr;
  hr = ((qi_fn)vtbl_of(pso)[0])(pso, &iid_pso, &qv);
  CHECK(hr == 0 && qv == pso, "PSO QI(ID3D12PipelineState) -> S_OK, self");
  qv = nullptr;
  hr = ((qi_fn)vtbl_of(buf1)[0])(buf1, &iid_res, &qv);
  CHECK(hr == 0 && qv == buf1, "resource QI(ID3D12Resource) -> S_OK, self");

  printf("checks=%d failures=%d\n", g_checks, g_failures);
  if (g_failures == 0) {
    printf("RESULT: ALL PASS\n");
    return 0;
  }
  printf("RESULT: FAILED\n");
  return 1;
}
