// tiramisu/tests/test_d3d12_rootsig.cpp
// Root-signature serialization round trip: D3D12SerializeRootSignature and
// D3D12SerializeVersionedRootSignature must carry the real layout, the two
// deserializers must hand the layout back, and CreateRootSignature must be able
// to see it. Before this the serializer returned S_OK with an empty container,
// so every assertion below would have failed except "returns S_OK".
#include "../src/trt/bridge_d3d12.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, what)                        \
  do {                                           \
    if (cond) {                                  \
      ++g_pass;                                  \
      printf("  PASS %s\n", what);                \
    } else {                                     \
      ++g_fail;                                  \
      printf("  FAIL %s\n", what);                \
    }                                            \
  } while (0)

// ---- guest ABI layouts (x86_64 xwin/MSVC d3d12.h) --------------------------
// Declared here so the compiler proves the bridge's hard-coded offsets.
struct t_range {  // D3D12_DESCRIPTOR_RANGE
  uint32_t type, num, reg, space, offset;
};
static_assert(sizeof(t_range) == 20, "D3D12_DESCRIPTOR_RANGE is 20 bytes");

struct t_range1 {  // D3D12_DESCRIPTOR_RANGE1
  uint32_t type, num, reg, space, flags, offset;
};
static_assert(sizeof(t_range1) == 24, "D3D12_DESCRIPTOR_RANGE1 is 24 bytes");

struct t_table {
  uint32_t num;
  uint32_t pad;
  const void* ranges;
};
static_assert(sizeof(t_table) == 16, "descriptor table is 16 bytes");

struct t_constants {
  uint32_t reg, space, num32;
};
struct t_desc {
  uint32_t reg, space;
};
struct t_desc1 {
  uint32_t reg, space, flags;
};

union t_param_union {
  t_table table;
  t_constants constants;
  t_desc descriptor;
  t_desc1 descriptor1;
};
static_assert(sizeof(t_param_union) == 16, "root parameter union is 16 bytes");

struct t_param {  // D3D12_ROOT_PARAMETER / ...PARAMETER1
  uint32_t type;
  t_param_union u;
  uint32_t visibility;
};
static_assert(sizeof(t_param) == 32, "D3D12_ROOT_PARAMETER is 32 bytes");

struct t_sampler {  // D3D12_STATIC_SAMPLER_DESC
  uint32_t filter, addr_u, addr_v, addr_w;
  float mip_lod_bias;
  uint32_t max_aniso, comparison, border;
  float min_lod, max_lod;
  uint32_t shader_register, register_space, visibility;
};
static_assert(sizeof(t_sampler) == 52, "D3D12_STATIC_SAMPLER_DESC is 52 bytes");

struct t_desc40 {  // D3D12_ROOT_SIGNATURE_DESC / ...DESC1 / ...DESC2
  uint32_t num_params;
  const t_param* params;
  uint32_t num_samplers;
  const t_sampler* samplers;
  uint32_t flags;
};
static_assert(sizeof(t_desc40) == 40, "D3D12_ROOT_SIGNATURE_DESC is 40 bytes");

struct t_vdesc {  // D3D12_VERSIONED_ROOT_SIGNATURE_DESC
  uint32_t version;
  uint32_t pad;
  t_desc40 body;
};
static_assert(sizeof(t_vdesc) == 48, "D3D12_VERSIONED_ROOT_SIGNATURE_DESC is 48 bytes");

// ---- COM call helpers ------------------------------------------------------
struct com_obj {
  const void* const* vtbl;
};

template <typename Fn>
static Fn slot(void* obj, unsigned idx) {
  return (Fn)((com_obj*)obj)->vtbl[idx];
}

typedef void* (TRT_GUEST_ABI* get_ptr_fn)(void*);
typedef uint64_t(TRT_GUEST_ABI* get_size_fn)(void*);
typedef uint32_t(TRT_GUEST_ABI* rel_fn)(void*);

typedef int(TRT_GUEST_ABI* serialize_fn)(const void*, uint32_t, void**, void**);
typedef int(TRT_GUEST_ABI* serialize_v_fn)(const void*, void**, void**);
typedef int(TRT_GUEST_ABI* make_deser_fn)(const void*, uint64_t, const void*, void**);
typedef const void*(TRT_GUEST_ABI* get_desc_fn)(void*);
typedef int(TRT_GUEST_ABI* get_at_version_fn)(void*, uint32_t, void**);
typedef const void*(TRT_GUEST_ABI* get_unconverted_fn)(void*);
typedef int(TRT_GUEST_ABI* create_device_fn)(void*, uint32_t, const void*, void**);
typedef int(TRT_GUEST_ABI* create_rootsig_fn)(void*, uint32_t, const void*, uint64_t,
                                              const void*, void**);

// A neutral REFIID: the deserializers echo back whatever IID they were created
// with, so the test supplies one instead of asserting an SDK GUID.
static const uint8_t k_test_iid[16] = {0x15, 0xae, 0x96, 0xe3, 0x55, 0xee, 0x2e, 0x4d,
                                       0xab, 0x4d, 0x0c, 0x7f, 0x9b, 0x8e, 0xe6, 0x0f};

int main() {
  printf("test_d3d12_rootsig\n");

  // ---- 1. Build a real 1.0 layout: one 32-bit-constant block, one table ----
  static t_range range{};
  range.type = 0;  // D3D12_DESCRIPTOR_RANGE_TYPE_SRV
  range.num = 4;
  range.reg = 2;
  range.space = 1;
  range.offset = 3;

  static t_param params[2]{};
  params[0].type = 1;  // D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS
  params[0].u.constants.reg = 0;
  params[0].u.constants.space = 0;
  params[0].u.constants.num32 = 2;
  params[0].visibility = 1;  // VERTEX

  params[1].type = 0;  // D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE
  params[1].u.table.num = 1;
  params[1].u.table.ranges = &range;
  params[1].visibility = 5;  // PIXEL

  static t_sampler sampler{};
  sampler.filter = 0x55;  // MIN_MAG_MIP_LINEAR
  sampler.addr_u = sampler.addr_v = sampler.addr_w = 1;  // WRAP
  sampler.comparison = 1;                                // NEVER
  sampler.max_lod = 1000.0f;
  sampler.shader_register = 0;
  sampler.register_space = 0;
  sampler.visibility = 0;  // ALL

  t_desc40 desc{};
  desc.num_params = 2;
  desc.params = params;
  desc.num_samplers = 1;
  desc.samplers = &sampler;
  desc.flags = 0x2;  // ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT

  void* blob = nullptr;
  void* err = nullptr;
  int hr = ((serialize_fn)&trt_D3D12SerializeRootSignature)(&desc, 1, &blob, &err);
  CHECK(hr == 0 && blob != nullptr, "SerializeRootSignature(real desc) -> S_OK + blob");
  CHECK(err == nullptr, "SerializeRootSignature returns no error blob on success");

  const uint32_t* bytes = blob ? (const uint32_t*)slot<get_ptr_fn>(blob, 3)(blob) : nullptr;
  const uint64_t size = blob ? slot<get_size_fn>(blob, 4)(blob) : 0;
  CHECK(bytes && bytes[0] == 0x43425844u, "blob begins with the DXBC magic");
  CHECK(size > 44, "blob carries a chunk table and payload, not a bare header");

  // The container must actually mention the layout, not just the version.
  bool saw_rdat = false, saw_stat = false;
  if (bytes) {
    const uint8_t* raw = (const uint8_t*)bytes;
    for (uint64_t i = 0; i + 4 <= size; ++i) {
      uint32_t tag = 0;
      memcpy(&tag, raw + i, 4);
      if (tag == 0x54414452u) saw_rdat = true;   // 'RDAT'
      if (tag == 0x54415453u) saw_stat = true;   // 'STAT'
    }
  }
  CHECK(saw_rdat, "blob contains an 'RDAT' parameter chunk");
  CHECK(saw_stat, "blob contains a 'STAT' static-sampler chunk");

  // ---- 2. Deserialize and inspect the layout ------------------------------
  void* deser = nullptr;
  hr = ((make_deser_fn)&trt_D3D12CreateRootSignatureDeserializer)(bytes, size, k_test_iid,
                                                                  &deser);
  CHECK(hr == 0 && deser != nullptr, "CreateRootSignatureDeserializer -> S_OK");
  if (deser) {
    const t_desc40* back = (const t_desc40*)slot<get_desc_fn>(deser, 3)(deser);
    CHECK(back != nullptr, "GetRootSignatureDesc returns a desc");
    CHECK(back && back->num_params == 2, "deserialized NumParameters == 2");
    CHECK(back && back->num_samplers == 1, "deserialized NumStaticSamplers == 1");
    CHECK(back && back->flags == 0x2, "deserialized Flags preserved (0x2)");
    if (back && back->num_params == 2) {
      CHECK(back->params[0].type == 1 && back->params[0].u.constants.reg == 0 &&
                back->params[0].u.constants.num32 == 2,
            "param 0 round-trips as 32-bit constants {reg 0, 2 values}");
      CHECK(back->params[1].type == 0 && back->params[1].u.table.num == 1 &&
                back->params[1].visibility == 5,
            "param 1 round-trips as a 1-range descriptor table (PIXEL)");
      if (back->params[1].u.table.num == 1 && back->params[1].u.table.ranges) {
        const t_range* r = (const t_range*)back->params[1].u.table.ranges;
        CHECK(r->type == 0 && r->num == 4 && r->reg == 2 && r->space == 1 &&
                  r->offset == 3,
              "descriptor range round-trips {SRV, 4, reg 2, space 1, offset 3}");
      }
    }
    if (back && back->num_samplers == 1 && back->samplers) {
      CHECK(back->samplers[0].filter == 0x55 && back->samplers[0].addr_u == 1 &&
                back->samplers[0].max_lod == 1000.0f,
            "static sampler round-trips filter/address/MaxLOD");
    }
    CHECK(slot<rel_fn>(deser, 2)(deser) == 0, "deserializer Release -> 0");
  }

  // QI for the IID it was created with succeeds; a foreign IID does not.
  void* deser2 = nullptr;
  hr = ((make_deser_fn)&trt_D3D12CreateRootSignatureDeserializer)(bytes, size, k_test_iid,
                                                                  &deser2);
  if (hr == 0 && deser2) {
    typedef int(TRT_GUEST_ABI * qi_fn)(void*, const void*, void**);
    void* same = nullptr;
    void* other = nullptr;
    uint8_t foreign[16] = {0};
    foreign[0] = 0x99;
    const int ok_same = slot<qi_fn>(deser2, 0)(deser2, k_test_iid, &same);
    const int ok_other = slot<qi_fn>(deser2, 0)(deser2, foreign, &other);
    CHECK(ok_same == 0 && same == deser2, "QueryInterface(created IID) -> S_OK");
    CHECK(ok_other != 0 && other == nullptr,
          "QueryInterface(foreign IID) -> E_NOINTERFACE, no pointer");
    if (ok_same == 0) slot<rel_fn>(deser2, 2)(deser2);
    slot<rel_fn>(deser2, 2)(deser2);
  }

  // ---- 3. The versioned path ----------------------------------------------
  static t_range1 range11{};
  range11.type = 2;  // CBV
  range11.num = 1;
  range11.reg = 4;
  range11.flags = 1;  // D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC
  static t_param vparams[1]{};
  vparams[0].type = 3;  // SRV
  vparams[0].u.descriptor1.reg = 5;
  vparams[0].u.descriptor1.space = 2;
  vparams[0].u.descriptor1.flags = 1;  // D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC
  vparams[0].visibility = 0;

  t_vdesc vdesc{};
  vdesc.version = 2;  // D3D_ROOT_SIGNATURE_VERSION_1_1
  vdesc.body.num_params = 1;
  vdesc.body.params = vparams;
  vdesc.body.num_samplers = 1;
  vdesc.body.samplers = &sampler;
  vdesc.body.flags = 0;

  void* vblob = nullptr;
  void* verr = nullptr;
  hr = ((serialize_v_fn)&trt_D3D12SerializeVersionedRootSignature)(&vdesc, &vblob, &verr);
  CHECK(hr == 0 && vblob != nullptr, "SerializeVersionedRootSignature(1.1) -> S_OK");
  const uint8_t* vbytes =
      vblob ? (const uint8_t*)slot<get_ptr_fn>(vblob, 3)(vblob) : nullptr;
  const uint64_t vsize = vblob ? slot<get_size_fn>(vblob, 4)(vblob) : 0;

  void* vdeser = nullptr;
  hr = ((make_deser_fn)&trt_D3D12CreateVersionedRootSignatureDeserializer)(
      vbytes, vsize, k_test_iid, &vdeser);
  CHECK(hr == 0 && vdeser != nullptr, "CreateVersionedRootSignatureDeserializer -> S_OK");
  if (vdeser) {
    const t_vdesc* unc = (const t_vdesc*)slot<get_unconverted_fn>(vdeser, 4)(vdeser);
    CHECK(unc && unc->version == 2, "GetUnconvertedRootSignatureDesc keeps version 1.1");
    CHECK(unc && unc->body.num_params == 1, "unconverted desc has 1 parameter");

    void* at10 = nullptr;
    hr = slot<get_at_version_fn>(vdeser, 3)(vdeser, 1, &at10);
    CHECK(hr == 0 && at10 != nullptr, "GetRootSignatureDescAtVersion(1.0) -> S_OK");
    if (at10) {
      const t_vdesc* c = (const t_vdesc*)at10;
      CHECK(c->version == 1, "converted desc reports version 1.0");
      CHECK(c->body.num_params == 1 && c->body.params[0].u.descriptor.reg == 5,
            "1.1 -> 1.0 conversion preserves the SRV register (flags dropped)");
    }
    void* at11 = nullptr;
    hr = slot<get_at_version_fn>(vdeser, 3)(vdeser, 2, &at11);
    CHECK(hr == 0 && at11 && ((const t_vdesc*)at11)->version == 2,
          "GetRootSignatureDescAtVersion(1.1) -> version 1.1 desc");
    slot<rel_fn>(vdeser, 2)(vdeser);
  }

  // ---- 4. Invalid descs must fail visibly with a reason -------------------
  t_desc40 bad = desc;
  bad.num_params = 65;  // above the D3D12 limit of 64
  void* bblob = nullptr;
  void* berr = nullptr;
  hr = ((serialize_fn)&trt_D3D12SerializeRootSignature)(&bad, 1, &bblob, &berr);
  CHECK(hr != 0 && bblob == nullptr, "65 root parameters -> failure, no blob");
  CHECK(berr != nullptr, "65 root parameters -> an error blob is returned");
  if (berr) {
    const char* text = (const char*)slot<get_ptr_fn>(berr, 3)(berr);
    CHECK(text && strstr(text, "NumParameters") != nullptr,
          "error blob text names NumParameters");
    slot<rel_fn>(berr, 2)(berr);
  }

  static t_param bad_params[1]{};
  bad_params[0].type = 9;  // not a D3D12_ROOT_PARAMETER_TYPE
  t_desc40 bad_type = desc;
  bad_type.num_params = 1;
  bad_type.params = bad_params;
  bad_type.num_samplers = 0;
  bad_type.samplers = nullptr;
  bblob = nullptr;
  berr = nullptr;
  hr = ((serialize_fn)&trt_D3D12SerializeRootSignature)(&bad_type, 1, &bblob, &berr);
  CHECK(hr != 0 && bblob == nullptr, "invalid ParameterType -> failure, no blob");
  if (berr) slot<rel_fn>(berr, 2)(berr);

  // ---- 5. CreateRootSignature sees the real layout ------------------------
  void* dev = nullptr;
  hr = ((create_device_fn)&trt_D3D12CreateDevice)(nullptr, 0xc000, nullptr, &dev);
  CHECK(hr == 0 && dev != nullptr, "D3D12CreateDevice(12_0) -> S_OK");
  if (dev) {
    void* rs = nullptr;
    hr = ((create_rootsig_fn)slot<create_rootsig_fn>(dev, 16))(dev, 0, bytes, size, nullptr,
                                                              &rs);
    CHECK(hr == 0 && rs != nullptr, "CreateRootSignature(serialized blob) -> S_OK");
    CHECK(trt_d3d12_rootsig_param_count(rs) == 2,
          "CreateRootSignature recovered 2 root parameters");
    CHECK(trt_d3d12_rootsig_static_sampler_count(rs) == 1,
          "CreateRootSignature recovered 1 static sampler");
    CHECK(trt_d3d12_rootsig_flags(rs) == 0x2, "CreateRootSignature recovered Flags 0x2");
    if (rs) slot<rel_fn>(rs, 2)(rs);

    // A hand-authored / foreign blob must still bind (corpus relies on it) and
    // simply report that its layout is not introspectable.
    static const uint8_t foreign_blob[16] = {0x44, 0x58, 0x42, 0x43, 1, 0, 0, 0,
                                             2,    0,    0,    0,    0, 0, 0, 0};
    void* rs2 = nullptr;
    hr = ((create_rootsig_fn)slot<create_rootsig_fn>(dev, 16))(dev, 0, foreign_blob,
                                                              sizeof(foreign_blob), nullptr,
                                                              &rs2);
    CHECK(hr == 0 && rs2 != nullptr, "CreateRootSignature(foreign blob) still succeeds");
    CHECK(trt_d3d12_rootsig_param_count(rs2) == 0,
          "foreign blob reports 0 introspectable parameters");
    if (rs2) slot<rel_fn>(rs2, 2)(rs2);
    slot<rel_fn>(dev, 2)(dev);
  }

  if (blob) slot<rel_fn>(blob, 2)(blob);
  if (vblob) slot<rel_fn>(vblob, 2)(vblob);

  printf("RESULT: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
