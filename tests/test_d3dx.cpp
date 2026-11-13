// tiramisu/tests/test_d3dx.cpp
// D3DX10/11 bridge, standalone: the bridge calls the guest device's
// CreateTexture2D (slot 5) / CreateShaderResourceView (slot 7) through the
// device's own vtable — this test installs a fake device whose vtable
// records the calls and verifies desc + pixels + SRV desc + info structs.
#include "../src/trt/trt.h"
#include "../src/trt/bridge_d3dx.h"
#include "../src/trt/thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// trt_core_log comes from src/trt/thunk.cpp (linked below)

namespace {

int failures = 0;
#define CHECK(cond, name)                                          \
  do {                                                             \
    if (cond) printf("  PASS %s\n", name);                         \
    else { printf("  FAIL %s\n", name); ++failures; }              \
  } while (0)

// ---- fake device: 48-slot vtable, slots 5 and 7 recorded ----
struct tex2d_desc {
  u32 width, height, mip_levels, array_size, format, sample_count,
      sample_quality, usage, bind_flags, cpu_access, misc;
};
struct subres_data { const void* p_sys_mem; u32 sys_mem_pitch, sys_mem_slice_pitch; };

static u32 g_tex_calls = 0;
static u32 g_srv_calls = 0;
static tex2d_desc g_last_desc;
static subres_data g_last_data;
static u8 g_px_store[64 * 64 * 4];  // snapshot: the bridge frees after the call
static u32 g_srv_dimension = 0;
static void* g_fake_tex = (void*)(uintptr_t)0x5A5A5A00;

int TRT_GUEST_ABI fake_CreateTexture2D(void* dev, const void* desc,
                                       const void* initial, void** out) {
  (void)dev;
  ++g_tex_calls;
  if (desc) g_last_desc = *(const tex2d_desc*)desc;
  if (initial) {
    g_last_data = *(const subres_data*)initial;
    const u32 n = g_last_desc.width * g_last_desc.height * 4;
    if (n <= sizeof(g_px_store) && g_last_data.p_sys_mem)
      memcpy(g_px_store, g_last_data.p_sys_mem, n);
  }
  if (out) *out = g_fake_tex;
  return 0;
}
int TRT_GUEST_ABI fake_CreateSRV(void* dev, void* res, const void* desc,
                                 void** out) {
  (void)dev; (void)res;
  ++g_srv_calls;
  if (desc) g_srv_dimension = ((const u32*)desc)[1];
  if (out) *out = (void*)(uintptr_t)0x33330000;
  return 0;
}

void* g_dev_vtbl[48];
void* g_dev_obj = g_dev_vtbl;  // device's first member is the vtable ptr

void setup_device() {
  memset(g_dev_vtbl, 0, sizeof(g_dev_vtbl));
  g_dev_vtbl[5] = (void*)&fake_CreateTexture2D;
  g_dev_vtbl[7] = (void*)&fake_CreateSRV;
}

// ---- minimal PNG (2x1, RGBA): stored-deflate zlib stream ----
u32 adler32(const u8* d, u32 n) {
  u32 a = 1, b = 0;
  for (u32 i = 0; i < n; ++i) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
  return (b << 16) | a;
}
u32 crc32_of(const u8* d, u32 n) {
  u32 crc = 0xFFFFFFFF;
  for (u32 i = 0; i < n; ++i) {
    crc ^= d[i];
    for (int k = 0; k < 8; ++k)
      crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
  }
  return crc ^ 0xFFFFFFFF;
}
u32 put_chunk(u8* o, u32 off, const char* type, const u8* payload, u32 len) {
  o[off] = (u8)(len >> 24); o[off+1] = (u8)(len >> 16);
  o[off+2] = (u8)(len >> 8); o[off+3] = (u8)len; off += 4;
  memcpy(o + off, type, 4); off += 4;
  memcpy(o + off, payload, len); off += len;
  const u32 crc = crc32_of(o + off - len - 4, len + 4);
  o[off] = (u8)(crc >> 24); o[off+1] = (u8)(crc >> 16);
  o[off+2] = (u8)(crc >> 8); o[off+3] = (u8)crc;
  return off + 4;
}
u32 build_png_2x1(u8* o) {
  static const u8 sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  memcpy(o, sig, 8);
  u32 off = 8;
  u8 ihdr[13] = {0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0};  // 2x1, d8, RGBA
  off = put_chunk(o, off, "IHDR", ihdr, 13);
  const u8 raw[10] = {0, 255, 128, 0, 200, 0, 10, 20, 30, 40};
  u8 z[24];
  z[0] = 0x78; z[1] = 0x01;
  z[2] = 0x01;  // BFINAL stored
  z[3] = 9; z[4] = 0; z[5] = (u8)~9; z[6] = 0xFF;
  memcpy(z + 7, raw, 9);
  const u32 ad = adler32(raw, 9);
  z[16] = (u8)(ad >> 24); z[17] = (u8)(ad >> 16); z[18] = (u8)(ad >> 8);
  z[19] = (u8)ad;
  off = put_chunk(o, off, "IDAT", z, 20);
  off = put_chunk(o, off, "IEND", (const u8*)"", 0);
  return off;
}

}  // namespace


// guest-facing exports under test (the bridge's header only exposes
// registration; declare the Win64 signatures here)
extern "C" int TRT_GUEST_ABI dx11_CreateSRVFromMemory(void*, const void*, u32,
                                                      void*, void**, void**,
                                                      void*);
extern "C" int TRT_GUEST_ABI dx11_GetImageInfoFromMemory(const void*, u32,
                                                         void*);
extern "C" int TRT_GUEST_ABI dx11_CreateTextureFromMemory(void*, const void*,
                                                          u32, void*, void**,
                                                          void*);
extern "C" int TRT_GUEST_ABI dx11_CreateTextureFromFileA(void*, const char*,
                                                         void*, void**,
                                                         void*);
extern "C" int TRT_GUEST_ABI dx10_CreateTextureFromMemory(void*, const void*,
                                                          u32, void*, void**,
                                                          void*);

int main(int, char**) {
  printf("tiramisu D3DX10/11 bridge test\n");
  setup_device();

  // ==================== PNG from memory -> texture + SRV ====================
  u8 png[256];
  const u32 png_len = build_png_2x1(png);
  void* tex = nullptr;
  void* srv = nullptr;
  int hr = 0x11111111;
  // declare the export shape directly (the bridge's own header only
  // exposes registration; these are the guest-facing signatures)
  hr = dx11_CreateSRVFromMemory(&g_dev_obj, png, png_len, nullptr, &tex, &srv,
                                &hr);
  CHECK(hr == 0, "PNG SRV-from-memory S_OK");
  CHECK(tex == g_fake_tex, "PNG texture = device's out pointer");
  CHECK(srv != nullptr, "PNG SRV created");
  CHECK(g_tex_calls == 1, "CreateTexture2D called once");
  CHECK(g_last_desc.width == 2 && g_last_desc.height == 1,
        "desc dims from PNG header");
  CHECK(g_last_desc.format == 28, "desc format R8G8B8A8_UNORM (28)");
  CHECK(g_last_desc.bind_flags == 8, "desc bind SHADER_RESOURCE");
  CHECK(g_last_desc.usage == 1, "desc usage IMMUTABLE");
  CHECK(g_last_data.sys_mem_pitch == 8, "subresource pitch = 2px * 4");
  const u8* px = g_px_store;  // snapshot taken inside CreateTexture2D
  CHECK(px[0] == 255 && px[1] == 128 && px[2] == 0 && px[3] == 200,
        "PNG pixel bytes exact");
  CHECK(g_srv_dimension == 4, "SRV view dimension TEXTURE2D (4)");

  // ==================== image-info pre-scan ====================
  u32 info[12];
  memset(info, 0xAA, sizeof(info));
  hr = dx11_GetImageInfoFromMemory(png, png_len, info);
  CHECK(hr == 0, "GetImageInfoFromMemory S_OK");
  CHECK(info[0] == 2 && info[1] == 1, "info width/height");
  CHECK(info[6] == 28, "info format R8G8B8A8");
  CHECK(info[7] == 4, "info resource type TEXTURE2D");

  // ==================== DDS DXT5 from memory ====================
  u8 dds[128 + 16];
  memset(dds, 0, sizeof(dds));
  memcpy(dds, "DDS ", 4);
  dds[12] = 4; dds[16] = 4;  // 4x4
  dds[80] = 4;               // DDPF_FOURCC
  memcpy(dds + 84, "DXT5", 4);
  memset(dds + 128, 0, 16);  // alpha 0..0, color 0..0 = black opaque-ish
  g_tex_calls = 0;
  hr = dx11_CreateTextureFromMemory(&g_dev_obj, dds, sizeof(dds), nullptr,
                                    &tex, &hr);
  CHECK(hr == 0 && g_tex_calls == 1, "DDS DXT5 texture created");
  CHECK(g_last_desc.width == 4 && g_last_desc.height == 4, "DDS dims 4x4");

  // ==================== file variants through the VFS ====================
  const char* tmp = "/tmp/opencode/test_d3dx_img.png";
  FILE* os = fopen(tmp, "wb");
  fwrite(png, 1, png_len, os);
  fclose(os);
  g_tex_calls = 0;
  hr = dx11_CreateTextureFromFileA(&g_dev_obj, tmp, nullptr, &tex, &hr);
  CHECK(hr == 0 && g_tex_calls == 1, "texture from FILE (A) works");
  // missing file fails named
  hr = dx11_CreateTextureFromFileA(&g_dev_obj, "/tmp/opencode/no_such.png",
                                   nullptr, &tex, &hr);
  CHECK(hr == 0x80070002, "missing file = HRESULT_FILE_NOT_FOUND");

  // ==================== d3dx10 alias ====================
  g_tex_calls = 0;
  hr = dx10_CreateTextureFromMemory(&g_dev_obj, png, png_len, nullptr, &tex,
                                    &hr);
  CHECK(hr == 0 && g_tex_calls == 1, "d3dx10 alias routes to the same path");

  // ==================== JPG fails named ====================
  hr = dx11_CreateTextureFromMemory(&g_dev_obj, "\xFF\xD8\xFF\xE0junk", 8,
                                    nullptr, &tex, &hr);
  CHECK(hr != 0, "JPG container fails named");

  if (failures == 0) {
    printf("RESULT: ALL PASS\n");
    return 0;
  }
  printf("RESULT: %d FAILURES\n", failures);
  return 1;
}
