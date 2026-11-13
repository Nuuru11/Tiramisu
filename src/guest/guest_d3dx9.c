// tiramisu/src/guest/guest_d3dx9.c
// Proves the D3DX9 bridge: exact matrix math, texture creation from a
// BMP in memory, sprite lifecycle, shader assembly. All calls through
// the IAT (module entry points, TRT_GUEST_ABI).
// Returns a bitmask:
//   0x001 MatrixIdentity exact
//   0x002 MatrixMultiply exact (known 2x2-in-4x4 product)
//   0x004 MatrixPerspectiveFovLH exact (90deg fov, aspect 1 -> x=y)
//   0x008 MatrixInverse exact (orthogonal rotation inverts to transpose)
//   0x010 MatrixTranspose exact
//   0x020 Vec3Normalize/Cross/Dot exact
//   0x040 CreateTexture (direct) returned a handle
//   0x080 CreateTextureFromFileInMemory (BMP) returned a handle
//   0x100 AssembleShader returned a buffer with the source bytes
//   0x200 CreateSprite returned an object; Begin/End ran
typedef unsigned int u32;
typedef int i32;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

typedef float mat4[16];
typedef float vec4[4];

__attribute__((dllimport)) void GSTD D3DXMatrixIdentity(mat4 out);
__attribute__((dllimport)) void GSTD D3DXMatrixTranslation(mat4 out, float x,
                                                           float y, float z);
__attribute__((dllimport)) void GSTD D3DXMatrixScaling(mat4 out, float x,
                                                       float y, float z);
__attribute__((dllimport)) void GSTD D3DXMatrixRotationX(mat4 out, float a);
__attribute__((dllimport)) void GSTD D3DXMatrixRotationY(mat4 out, float a);
__attribute__((dllimport)) void GSTD D3DXMatrixRotationZ(mat4 out, float a);
__attribute__((dllimport)) void GSTD D3DXMatrixMultiply(mat4 out,
                                                        const mat4 a,
                                                        const mat4 b);
__attribute__((dllimport)) void GSTD D3DXMatrixPerspectiveFovLH(
    mat4 out, float fovy, float aspect, float zn, float zf);
__attribute__((dllimport)) void GSTD D3DXMatrixInverse(mat4 out,
                                                       float* det,
                                                       const mat4 m);
__attribute__((dllimport)) void GSTD D3DXMatrixTranspose(mat4 out,
                                                         const mat4 m);
__attribute__((dllimport)) void GSTD D3DXVec3Normalize(vec4 out,
                                                       const vec4 v);
__attribute__((dllimport)) void GSTD D3DXVec3Cross(vec4 out, const vec4 a,
                                                   const vec4 b);
__attribute__((dllimport)) float GSTD D3DXVec3Dot(const vec4 a,
                                                  const vec4 b);
__attribute__((dllimport)) u32 GSTD D3DXCreateTexture(void* dev, u32 w,
                                                      u32 h, u32 levels,
                                                      u32 usage, u32 fmt,
                                                      u32 pool,
                                                      void** out_tex);
__attribute__((dllimport)) u32 GSTD D3DXCreateTextureFromFileInMemory(
    void* dev, const void* data, u32 bytes, void** out_tex);
__attribute__((dllimport)) u32 GSTD D3DXAssembleShader(
    const char* src, u32 len, const void* defines, const void* includes,
    u32 flags, void** code, void** errors);
__attribute__((dllimport)) u32 GSTD D3DXCreateSprite(void* dev,
                                                     void** out_sprite);

static int feq(float a, float b) {
  const float d = a - b;
  return (d < 0.0f ? -d : d) < 0.0001f;
}

u32 GSTD guest_d3dx9_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;
  void* dev = (void*)0x1234;  // the bridge ignores the device pointer

  // 1. identity
  mat4 m;
  D3DXMatrixIdentity(m);
  if (m[0] == 1.0f && m[5] == 1.0f && m[10] == 1.0f && m[15] == 1.0f &&
      m[1] == 0.0f && m[12] == 0.0f)
    bits |= 0x001u;

  // 2. multiply: translation * scale
  mat4 t, s, r;
  D3DXMatrixTranslation(t, 5.0f, 0.0f, 0.0f);
  D3DXMatrixScaling(s, 2.0f, 2.0f, 2.0f);
  D3DXMatrixMultiply(r, s, t);
  // D3D row-vector: out = s*t transforms scale-first, then translate
  if (r[0] == 2.0f && r[5] == 2.0f && r[10] == 2.0f && r[12] == 5.0f &&
      r[15] == 1.0f)
    bits |= 0x002u;

  // 3. perspective fov LH: 90 degrees, aspect 1 -> x == y; z in [0,1]
  const float pi = 3.14159265f;
  D3DXMatrixPerspectiveFovLH(r, pi / 2.0f, 1.0f, 1.0f, 100.0f);
  if (feq(r[0], r[5]) && feq(r[10], 100.0f / 99.0f) &&
      feq(r[14], -100.0f / 99.0f) && r[11] == 1.0f)
    bits |= 0x004u;

  // 4. inverse of a pure rotation = transpose, det = 1
  D3DXMatrixRotationZ(t, pi / 2.0f);
  float det = 0.0f;
  D3DXMatrixInverse(r, &det, t);
  if (feq(det, 1.0f) && feq(r[1], t[4]) && feq(r[4], t[1]) &&
      feq(r[0], 0.0f) && feq(r[5], 0.0f))
    bits |= 0x008u;

  // 5. transpose
  D3DXMatrixTranspose(r, t);
  if (feq(r[1], t[4]) && feq(r[4], t[1]) && r[15] == 1.0f)
    bits |= 0x010u;

  // 6. vector math: normalize (3,4,0) -> (0.6,0.8,0); cross e_x^e_y = e_z
  vec4 v = {3.0f, 4.0f, 0.0f, 0.0f};
  vec4 o;
  D3DXVec3Normalize(o, v);
  if (feq(o[0], 0.6f) && feq(o[1], 0.8f)) {
    vec4 x = {1.0f, 0.0f, 0.0f, 0.0f};
    vec4 y = {0.0f, 1.0f, 0.0f, 0.0f};
    D3DXVec3Cross(o, x, y);
    if (feq(o[2], 1.0f) && feq(D3DXVec3Dot(x, y), 0.0f))
      bits |= 0x020u;
  }

  // 7. D3DXCreateTexture direct
  void* tex = 0;
  if (D3DXCreateTexture(dev, 64, 64, 1, 0, 21, 1, &tex) == 0 && tex)
    bits |= 0x040u;

  // 8. texture from a BMP in memory (32-bit 4x4 red pixels)
  u8 bmp[86];
  for (u32 i = 0; i < 86; ++i) bmp[i] = 0;
  bmp[0] = 'B'; bmp[1] = 'M';
  *(u32*)(bmp + 2) = 86;    // file size
  *(u32*)(bmp + 10) = 54;   // pixel offset
  *(u32*)(bmp + 14) = 40;   // BITMAPINFOHEADER size
  *(i32*)(bmp + 18) = 4;    // width
  *(i32*)(bmp + 22) = 4;    // height (positive = bottom-up)
  *(u16*)(bmp + 26) = 1;    // planes
  *(u16*)(bmp + 28) = 32;   // bpp
  for (u32 i = 0; i < 64; ++i) bmp[54 + i] = 0xA0;  // some pixels
  tex = 0;
  if (D3DXCreateTextureFromFileInMemory(dev, bmp, 86, &tex) == 0 && tex)
    bits |= 0x080u;

  // 9. shader assembly round-trip: buffer carries the source bytes
  static const char shader[] = "vs_2_0 def c0, 1, 2, 3, 4";
  void* code = 0;
  void* errs = 0;
  if (D3DXAssembleShader(shader, (u32)sizeof(shader) - 1, 0, 0, 0, &code,
                         &errs) == 0 &&
      code) {
    // ID3DXBuffer: GetBufferPointer slot 3, GetBufferSize slot 4
    u8** vtbl = *(u8***)code;
    const char* bp = ((const char* GSTD (*)(void*))vtbl[3])(code);
    const u32 bs = ((u32 GSTD (*)(void*))vtbl[4])(code);
    if (bp && bs == sizeof(shader) - 1 && bp[0] == 'v' && bp[1] == 's' &&
        bp[2] == '_')
      bits |= 0x100u;
  }

  // 10. sprite lifecycle
  void* spr = 0;
  if (D3DXCreateSprite(dev, &spr) == 0 && spr) {
    u8** vtbl = *(u8***)spr;
    // Begin = slot 8, End = slot 11 (SDK order)
    const u32 b1 = ((u32 GSTD (*)(void*))vtbl[8])(spr);
    const u32 b2 = ((u32 GSTD (*)(void*))vtbl[11])(spr);
    if (b1 == 0 && b2 == 0) bits |= 0x200u;
  }

  return bits;
}
