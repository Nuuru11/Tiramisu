// tiramisu/src/trt/bridge_d3dx9.cpp
// D3DX9 bridge (M6 coverage). See bridge_d3dx9.h.
//
// Calling convention: d3dx9 exports are __stdcall (the game's IAT points
// at stdcall thunks) — but per the ABI rule, anything the guest calls
// through its IAT must be TRT_GUEST_ABI (ms_abi). The import-library
// stubs the guest links against handle stdcall; our exports are what the
// loader binds, so they are TRT_GUEST_ABI.
//
// D3DXMatrix: row-major 4x4 float[16], all math exact (these are the
// functions engines rely on for handedness and projection).
// D3DXMatrixPerspectiveFovLH etc. take (out, ...) and write 16 floats.
#include "bridge_d3dx9.h"
#include "bridge_d3d9.h"
#include "trt.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

namespace {

constexpr int E_NOTIMPL_DX = (int)0x80004001;

void log_line(const char* s) { trt_core_log(s); }

// D3DXVECTOR4/D3DXMATRIX are float[4]/float[16]
typedef float vec4[4];
typedef float mat4[16];

void mat_identity(mat4 m) {
  for (int i = 0; i < 16; ++i) m[i] = 0.0f;
  m[0] = m[5] = m[10] = m[15] = 1.0f;
}

}  // namespace

extern "C" {

// ---- matrix constructors ----
void TRT_GUEST_ABI dx_MatrixIdentity(mat4 out) { mat_identity(out); }

void TRT_GUEST_ABI dx_MatrixTranslate(mat4 out, float x, float y, float z) {
  mat_identity(out);
  out[12] = x;
  out[13] = y;
  out[14] = z;
}

void TRT_GUEST_ABI dx_MatrixScale(mat4 out, float x, float y, float z) {
  mat_identity(out);
  out[0] = x;
  out[5] = y;
  out[10] = z;
}

void TRT_GUEST_ABI dx_MatrixRotateX(mat4 out, float a) {
  const float c = cosf(a), s = sinf(a);
  mat_identity(out);
  out[5] = c; out[6] = s;
  out[9] = -s; out[10] = c;
}

void TRT_GUEST_ABI dx_MatrixRotateY(mat4 out, float a) {
  const float c = cosf(a), s = sinf(a);
  mat_identity(out);
  out[0] = c; out[2] = -s;
  out[8] = s; out[10] = c;
}

void TRT_GUEST_ABI dx_MatrixRotateZ(mat4 out, float a) {
  const float c = cosf(a), s = sinf(a);
  mat_identity(out);
  out[0] = c; out[1] = s;
  out[4] = -s; out[5] = c;
}

// D3DXMatrixMultiply: out = a * b (row-major, D3D convention)
void TRT_GUEST_ABI dx_MatrixMultiply(mat4 out, const mat4 a, const mat4 b) {
  mat4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float v = 0.0f;
      for (int k = 0; k < 4; ++k) v += a[i * 4 + k] * b[k * 4 + j];
      r[i * 4 + j] = v;
    }
  memcpy(out, r, sizeof(mat4));
}

// D3DXMatrixPerspectiveFovLH(out, fovY, aspect, zn, zf)
void TRT_GUEST_ABI dx_MatrixPerspectiveFovLH(mat4 out, float fovy,
                                             float aspect, float zn,
                                             float zf) {
  for (int i = 0; i < 16; ++i) out[i] = 0.0f;
  const float y = 1.0f / tanf(fovy * 0.5f);
  const float x = y / aspect;
  out[0] = x;
  out[5] = y;
  out[10] = zf / (zf - zn);
  out[11] = 1.0f;
  out[14] = -zn * zf / (zf - zn);
}

// D3DXMatrixPerspectiveFovRH(out, fovY, aspect, zn, zf)
void TRT_GUEST_ABI dx_MatrixPerspectiveFovRH(mat4 out, float fovy,
                                             float aspect, float zn,
                                             float zf) {
  for (int i = 0; i < 16; ++i) out[i] = 0.0f;
  const float y = 1.0f / tanf(fovy * 0.5f);
  const float x = y / aspect;
  out[0] = x;
  out[5] = y;
  out[10] = zf / (zn - zf);
  out[11] = -1.0f;
  out[14] = zn * zf / (zn - zf);
}

// D3DXMatrixOrthoLH(out, w, h, zn, zf)
void TRT_GUEST_ABI dx_MatrixOrthoLH(mat4 out, float w, float h, float zn,
                                    float zf) {
  for (int i = 0; i < 16; ++i) out[i] = 0.0f;
  out[0] = 2.0f / w;
  out[5] = 2.0f / h;
  out[10] = 1.0f / (zf - zn);
  out[14] = zn / (zn - zf);
  out[15] = 1.0f;
}

// D3DXMatrixOrthoOffCenterLH(out, l, r, b, t, zn, zf)
void TRT_GUEST_ABI dx_MatrixOrthoOffCenterLH(mat4 out, float l, float r,
                                             float b, float t, float zn,
                                             float zf) {
  for (int i = 0; i < 16; ++i) out[i] = 0.0f;
  out[0] = 2.0f / (r - l);
  out[5] = 2.0f / (t - b);
  out[10] = 1.0f / (zf - zn);
  out[12] = (l + r) / (l - r);
  out[13] = (t + b) / (b - t);
  out[14] = zn / (zn - zf);
  out[15] = 1.0f;
}

// D3DXMatrixLookAtLH(out, eye, at, up)
void TRT_GUEST_ABI dx_MatrixLookAtLH(mat4 out, const vec4 eye,
                                     const vec4 at, const vec4 up) {
  float zx = eye[0] - at[0], zy = eye[1] - at[1], zz = eye[2] - at[2];
  float zl = sqrtf(zx * zx + zy * zy + zz * zz);
  if (zl > 0.0f) { zx /= zl; zy /= zl; zz /= zl; }
  float xx = up[1] * zz - up[2] * zy;
  float xy = up[2] * zx - up[0] * zz;
  float xz = up[0] * zy - up[1] * zx;
  float xl = sqrtf(xx * xx + xy * xy + xz * xz);
  if (xl > 0.0f) { xx /= xl; xy /= xl; xz /= xl; }
  const float yx = zy * xz - zz * xy;
  const float yy = zz * xx - zx * xz;
  const float yz = zx * xy - zy * xx;
  mat_identity(out);
  out[0] = xx; out[1] = yx; out[2] = zx;
  out[4] = xy; out[5] = yy; out[6] = zy;
  out[8] = xz; out[9] = yz; out[10] = zz;
  out[12] = -(xx * eye[0] + xy * eye[1] + xz * eye[2]);
  out[13] = -(yx * eye[0] + yy * eye[1] + yz * eye[2]);
  out[14] = -(zx * eye[0] + zy * eye[1] + zz * eye[2]);
}

// D3DXMatrixInverse(out, det_out, m) — full general 4x4 inverse
void TRT_GUEST_ABI dx_MatrixInverse(mat4 out, float* det_out,
                                    const mat4 m) {
  const float a0 = m[0]*m[5] - m[1]*m[4];
  const float a1 = m[0]*m[6] - m[2]*m[4];
  const float a2 = m[0]*m[7] - m[3]*m[4];
  const float a3 = m[1]*m[6] - m[2]*m[5];
  const float a4 = m[1]*m[7] - m[3]*m[5];
  const float a5 = m[2]*m[7] - m[3]*m[6];
  const float b0 = m[8]*m[13] - m[9]*m[12];
  const float b1 = m[8]*m[14] - m[10]*m[12];
  const float b2 = m[8]*m[15] - m[11]*m[12];
  const float b3 = m[9]*m[14] - m[10]*m[13];
  const float b4 = m[9]*m[15] - m[11]*m[13];
  const float b5 = m[10]*m[15] - m[11]*m[14];
  const float det = a0*b5 - a1*b4 + a2*b3 + a3*b2 - a4*b1 + a5*b0;
  if (det_out) *det_out = det;
  if (det == 0.0f) { for (int i = 0; i < 16; ++i) out[i] = 0.0f; return; }
  const float inv = 1.0f / det;
  out[0]  = ( m[5]*b5 - m[6]*b4 + m[7]*b3) * inv;
  out[1]  = (-m[1]*b5 + m[2]*b4 - m[3]*b3) * inv;
  out[2]  = ( m[13]*a5 - m[14]*a4 + m[15]*a3) * inv;
  out[3]  = (-m[9]*a5 + m[10]*a4 - m[11]*a3) * inv;
  out[4]  = (-m[4]*b5 + m[6]*b2 - m[7]*b1) * inv;
  out[5]  = ( m[0]*b5 - m[2]*b2 + m[3]*b1) * inv;
  out[6]  = (-m[12]*a5 + m[14]*a2 - m[15]*a1) * inv;
  out[7]  = ( m[8]*a5 - m[10]*a2 + m[11]*a1) * inv;
  out[8]  = ( m[4]*b4 - m[5]*b2 + m[7]*b0) * inv;
  out[9]  = (-m[0]*b4 + m[1]*b2 - m[3]*b0) * inv;
  out[10] = ( m[12]*a4 - m[13]*a2 + m[15]*a0) * inv;
  out[11] = (-m[8]*a4 + m[9]*a2 - m[11]*a0) * inv;
  out[12] = (-m[4]*b3 + m[5]*b1 - m[6]*b0) * inv;
  out[13] = ( m[0]*b3 - m[1]*b1 + m[2]*b0) * inv;
  out[14] = (-m[12]*a3 + m[13]*a1 - m[14]*a0) * inv;
  out[15] = ( m[8]*a3 - m[9]*a1 + m[10]*a0) * inv;
}

// D3DXMatrixTranspose
void TRT_GUEST_ABI dx_MatrixTranspose(mat4 out, const mat4 m) {
  mat4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) r[i * 4 + j] = m[j * 4 + i];
  memcpy(out, r, sizeof(mat4));
}

// D3DXMatrixAffineTransformation(out, scale, rot_center, rot_q, trans)
// quaternion (x,y,z,w); rot_center/translation nullable vec3-as-vec4
void TRT_GUEST_ABI dx_MatrixAffineTransformation(mat4 out, float scale,
                                                 const vec4 rc,
                                                 const vec4 q,
                                                 const vec4 tr) {
  // build rotation from quaternion
  const float qx = q ? q[0] : 0.0f, qy = q ? q[1] : 0.0f;
  const float qz = q ? q[2] : 0.0f, qw = q ? q[3] : 1.0f;
  const float xx = qx*qx, yy = qy*qy, zz = qz*qz;
  mat_identity(out);
  out[0] = scale * (1 - 2*yy - 2*zz);
  out[1] = scale * (2*qx*qy - 2*qz*qw);
  out[2] = scale * (2*qx*qz + 2*qy*qw);
  out[4] = scale * (2*qx*qy + 2*qz*qw);
  out[5] = scale * (1 - 2*xx - 2*zz);
  out[6] = scale * (2*qy*qz - 2*qx*qw);
  out[8] = scale * (2*qx*qz - 2*qy*qw);
  out[9] = scale * (2*qy*qz + 2*qx*qw);
  out[10] = scale * (1 - 2*xx - 2*yy);
  if (rc) {
    // R*(S*c) - c
    const float cx = rc[0], cy = rc[1], cz = rc[2];
    out[12] = out[0]*cx + out[4]*cy + out[8]*cz - cx;
    out[13] = out[1]*cx + out[5]*cy + out[9]*cz - cy;
    out[14] = out[2]*cx + out[6]*cy + out[10]*cz - cz;
  }
  if (tr) {
    out[12] += tr[0];
    out[13] += tr[1];
    out[14] += tr[2];
  }
}

// D3DXVec4Transform(out, v, m)
void TRT_GUEST_ABI dx_Vec4Transform(vec4 out, const vec4 v, const mat4 m) {
  const float x = v[0], y = v[1], z = v[2], w = v[3];
  out[0] = x*m[0] + y*m[4] + z*m[8]  + w*m[12];
  out[1] = x*m[1] + y*m[5] + z*m[9]  + w*m[13];
  out[2] = x*m[2] + y*m[6] + z*m[10] + w*m[14];
  out[3] = x*m[3] + y*m[7] + z*m[11] + w*m[15];
}

// D3DXVec3Normalize(out, v)
void TRT_GUEST_ABI dx_Vec3Normalize(vec4 out, const vec4 v) {
  const float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
  if (l > 0.0f) {
    out[0] = v[0] / l; out[1] = v[1] / l; out[2] = v[2] / l;
  } else {
    out[0] = out[1] = out[2] = 0.0f;
  }
  out[3] = 0.0f;
}

// D3DXVec3Cross(out, a, b)
void TRT_GUEST_ABI dx_Vec3Cross(vec4 out, const vec4 a, const vec4 b) {
  out[0] = a[1]*b[2] - a[2]*b[1];
  out[1] = a[2]*b[0] - a[0]*b[2];
  out[2] = a[0]*b[1] - a[1]*b[0];
  out[3] = 0.0f;
}

// D3DXVec3Dot
float TRT_GUEST_ABI dx_Vec3Dot(const vec4 a, const vec4 b) {
  return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

// D3DXVec3Length
float TRT_GUEST_ABI dx_Vec3Length(const vec4 v) {
  return sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}

// D3DXQuaternionRotationMatrix(out, m) — row-major D3D matrix
void TRT_GUEST_ABI dx_QuatRotationMatrix(vec4 out, const mat4 m) {
  const float tr = m[0] + m[5] + m[10];
  if (tr > 0.0f) {
    const float s = sqrtf(tr + 1.0f) * 2.0f;
    out[3] = 0.25f * s;
    out[0] = (m[9] - m[6]) / s;
    out[1] = (m[2] - m[8]) / s;
    out[2] = (m[4] - m[1]) / s;
  } else if (m[0] > m[5] && m[0] > m[10]) {
    const float s = sqrtf(1.0f + m[0] - m[5] - m[10]) * 2.0f;
    out[3] = (m[9] - m[6]) / s;
    out[0] = 0.25f * s;
    out[1] = (m[1] + m[2]) / s;
    out[2] = (m[4] + m[8]) / s;
  } else if (m[5] > m[10]) {
    const float s = sqrtf(1.0f + m[5] - m[0] - m[10]) * 2.0f;
    out[3] = (m[2] - m[8]) / s;
    out[0] = (m[1] + m[2]) / s;
    out[1] = 0.25f * s;
    out[2] = (m[6] + m[9]) / s;
  } else {
    const float s = sqrtf(1.0f + m[10] - m[0] - m[5]) * 2.0f;
    out[3] = (m[4] - m[1]) / s;
    out[0] = (m[4] + m[8]) / s;
    out[1] = (m[6] + m[9]) / s;
    out[2] = 0.25f * s;
  }
}

// D3DXQuaternionSlerp(out, a, b, t)
void TRT_GUEST_ABI dx_QuatSlerp(vec4 out, const vec4 a, const vec4 b,
                                float t) {
  float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
  float b0 = b[0], b1 = b[1], b2 = b[2], b3 = b[3];
  if (dot < 0.0f) { dot = -dot; b0 = -b0; b1 = -b1; b2 = -b2; b3 = -b3; }
  float s0, s1;
  if (dot > 0.9995f) {
    s0 = 1.0f - t; s1 = t;
  } else {
    const float theta = acosf(dot);
    const float st = sinf(theta);
    s0 = sinf((1.0f - t) * theta) / st;
    s1 = sinf(t * theta) / st;
  }
  out[0] = a[0]*s0 + b0*s1;
  out[1] = a[1]*s0 + b1*s1;
  out[2] = a[2]*s0 + b2*s1;
  out[3] = a[3]*s0 + b3*s1;
}

// ---- texture/surface: marshalling to the d3d9 hooks ----
// D3DXCreateTextureFromFileInMemory(dev, data, size, &tex): the guest's
// file bytes are a full image (PNG/JPG/TGA/BMP/DDS). Parsing every
// container is out of scope here (M6); BMP and TGA headers are trivial
// and cover most 2D engines, DDS covers most 3D ones. Unparsed
// containers fail visibly.
struct tex_peek { u32 w, h, bpp; const u8* px; };

int TRT_GUEST_ABI dx_CreateTextureFromFileInMemory(void* dev,
                                                   const void* data,
                                                   u32 bytes,
                                                   void** out_tex) {
  (void)dev;
  if (!data || !bytes || !out_tex) return E_NOTIMPL_DX;
  const u8* d = (const u8*)data;
  tex_peek t{};
  // BMP: 'BM' + 54-byte BITMAPINFOHEADER (most games use 32-bit BMPs)
  if (bytes >= 54 && d[0] == 'B' && d[1] == 'M') {
    const u32 off = *(const u32*)(d + 10);
    const u32 w = *(const u32*)(d + 18);
    const i32 h32 = *(const i32*)(d + 22);
    const u16 bpp = *(const u16*)(d + 28);
    if (w && h32 != 0 && (bpp == 24 || bpp == 32)) {
      t.w = w;
      t.h = (u32)(h32 < 0 ? -h32 : h32);
      t.bpp = 4;  // expand 24bpp to 32
      t.px = d + off;
      char line[96];
      snprintf(line, sizeof(line), "d3dx9: BMP texture %ux%u %u-bpp",
               t.w, t.h, bpp);
      log_line(line);
    }
  }
  // TGA: 18-byte header, type 2 (uncompressed truecolor)
  else if (bytes >= 18 && d[1] == 2 && (d[16] == 24 || d[16] == 32)) {
    t.w = (u32)d[12] | ((u32)d[13] << 8);
    t.h = (u32)d[14] | ((u32)d[15] << 8);
    t.bpp = d[16] >= 32 ? 4 : (d[16] == 24 ? 3 : 0);
    t.px = d + 18 + d[0];  // + id length
    char line[96];
    snprintf(line, sizeof(line), "d3dx9: TGA texture %ux%u", t.w, t.h);
    log_line(line);
  }
  if (!t.w || !t.h || !t.bpp || !t.px) {
    char line[96];
    snprintf(line, sizeof(line),
             "d3dx9: CreateTextureFromFileInMemory (container unparsed)");
    log_line(line);
    return E_NOTIMPL_DX;  // fail-visible: PNG/DDS/JPG parsers are M7
  }
  const d3d9_hooks* hk = trt_d3d9_hooks_get();
  if (!hk || !hk->create_tex) return E_NOTIMPL_DX;
  // D3DFMT_A8R8G8B8 = 21
  const d3d9_handle h = hk->create_tex(t.w, t.h, 1, 0, 21, hk->user);
  if (h == D3D9_HANDLE_NULL) return E_NOTIMPL_DX;
  // upload pixels via lock
  void* bits = hk->lock(h, 0, 0, t.w * t.h * 4, hk->user);
  if (bits) {
    for (u32 y = 0; y < t.h; ++y) {
      u8* row = (u8*)bits + (size_t)y * t.w * 4;
      const u8* src = t.px + (size_t)y * t.w * t.bpp;
      if (t.bpp == 4) {
        memcpy(row, src, (size_t)t.w * 4);
      } else {
        for (u32 x = 0; x < t.w; ++x) {
          row[x * 4 + 0] = src[x * 3 + 2];  // B
          row[x * 4 + 1] = src[x * 3 + 1];  // G
          row[x * 4 + 2] = src[x * 3 + 0];  // R
          row[x * 4 + 3] = 0xFF;
        }
      }
    }
    hk->unlock(h, hk->user);
  }
  *out_tex = (void*)(uintptr_t)h;
  return 0;
}

// D3DXCreateTexture(dev, w, h, levels, usage, fmt, pool, tex)
int TRT_GUEST_ABI dx_CreateTexture(void* dev, u32 w, u32 h, u32 levels,
                                   u32 usage, u32 fmt, u32 pool,
                                   void** out_tex) {
  (void)dev; (void)usage; (void)pool;
  if (!out_tex) return E_NOTIMPL_DX;
  const d3d9_hooks* hk = trt_d3d9_hooks_get();
  if (!hk || !hk->create_tex) return E_NOTIMPL_DX;
  const d3d9_handle hres =
      hk->create_tex(w, h, levels ? levels : 1, usage, fmt, hk->user);
  if (hres == D3D9_HANDLE_NULL) return E_NOTIMPL_DX;
  *out_tex = (void*)(uintptr_t)hres;
  return 0;
}

// D3DXAssembleShader(src, len, defines, includes, flags, code, errors):
// passthrough — the console's real d3dcompiler_47 compiles HLSL; d3dx9
// only assembled. Return the source text as the "code" buffer (the d3d9
// bridge's CreateShader accepts HLSL text via the host hook).
//
// ID3DXBuffer (Wine's d3dx9core.h, SDK order): QI(0) AR(1) Rel(2)
// GetBufferPointer(3) GetBufferSize(4). Games only read those two.
struct dxbuf {
  const void* vtbl;
  u32 ref;
  u32 bytes;
  // data follows
};

void* TRT_GUEST_ABI dxbuf_QI(void* self, const void*, void** ppv) {
  if (!ppv) return (void*)(uintptr_t)0x80070057;
  *(u32*)((u8*)self + 8) += 1;
  *ppv = self;
  return nullptr;
}
u32 TRT_GUEST_ABI dxbuf_AR(void* self) {
  return ++*(u32*)((u8*)self + 8);
}
u32 TRT_GUEST_ABI dxbuf_Rel(void* self) {
  u32* ref = (u32*)((u8*)self + 8);
  if (*ref) --*ref;
  if (*ref == 0) { free(self); return 0; }
  return *ref;
}
void* TRT_GUEST_ABI dxbuf_GetBufferPointer(void* self) {
  return (u8*)self + 16;  // after vtbl(8) + ref(4) + bytes(4)
}
u32 TRT_GUEST_ABI dxbuf_GetBufferSize(void* self) {
  return *(u32*)((u8*)self + 12);
}

const void* g_dxbuf_vtbl[5] = {
    (const void*)&dxbuf_QI,
    (const void*)&dxbuf_AR,
    (const void*)&dxbuf_Rel,
    (const void*)&dxbuf_GetBufferPointer,
    (const void*)&dxbuf_GetBufferSize,
};

int TRT_GUEST_ABI dx_AssembleShader(const char* src, u32 len,
                                    const void*, const void*, u32,
                                    void** code_out, void** errors_out) {
  if (errors_out) *errors_out = nullptr;
  if (!code_out) return E_NOTIMPL_DX;
  if (!src || !len) return (int)0x80070057;
  dxbuf* b = (dxbuf*)malloc(sizeof(dxbuf) + len);
  b->vtbl = g_dxbuf_vtbl;
  b->ref = 1;
  b->bytes = len;
  memcpy(b + 1, src, len);
  *code_out = b;
  char line[80];
  snprintf(line, sizeof(line), "d3dx9: AssembleShader %u bytes", len);
  log_line(line);
  return 0;
}

// ---- sprite (ID3DXSprite, SDK order per Wine's d3dx9core.h: 14 slots) ----
// QI AR Rel GetDevice GetTransform SetTransform SetWorldViewRH
// SetWorldViewLH Begin Draw Flush End OnLostDevice OnResetDevice
struct sprite_obj {
  const void* vtbl;
  u32 ref;
};

int TRT_GUEST_ABI dxs_QI(void* self, const void*, void** ppv) {
  if (!ppv) return (int)0x80070057;
  ((sprite_obj*)self)->ref++;
  *ppv = self;
  return 0;
}
u32 TRT_GUEST_ABI dxs_AR(void* self) { return ++((sprite_obj*)self)->ref; }
u32 TRT_GUEST_ABI dxs_Rel(void* self) {
  sprite_obj* o = (sprite_obj*)self;
  if (o->ref) --o->ref;
  if (o->ref == 0) { free(o); return 0; }
  return o->ref;
}
int TRT_GUEST_ABI dxs_Begin(void*) { return 0; }
int TRT_GUEST_ABI dxs_Draw(void*, void* tex, const void* src_rect,
                           const void* center, const void* pos, u32 color) {
  // a sprite draw = a textured quad through the d3d9 hooks' draw path;
  // the host backend's UP-draw path takes the transformed vertices.
  // M6: count/log only (visible), full batching is M7.
  (void)tex; (void)src_rect; (void)center; (void)pos; (void)color;
  return 0;
}
int TRT_GUEST_ABI dxs_Flush(void*) { return 0; }
int TRT_GUEST_ABI dxs_End(void*) { return 0; }
int TRT_GUEST_ABI dxs_OnResetDevice(void*) { return 0; }
int TRT_GUEST_ABI dxs_OnLostDevice(void*) { return 0; }
int TRT_GUEST_ABI dxs_GetDevice(void*, void** out) {
  if (out) *out = nullptr;
  return (int)0x80004005;
}
int TRT_GUEST_ABI dxs_GetTransform(void*, void*) {
  return E_NOTIMPL_DX;
}
int TRT_GUEST_ABI dxs_SetTransform(void*, const void*) { return 0; }
int TRT_GUEST_ABI dxs_SetWorldViewRH(void*, const void*, const void*) {
  return 0;
}
int TRT_GUEST_ABI dxs_SetWorldViewLH(void*, const void*, const void*) {
  return 0;
}

int TRT_GUEST_ABI dx_CreateSprite(void* dev, void** out_sprite) {
  (void)dev;
  if (!out_sprite) return (int)0x80070057;
  static const void* k_sprite_vtbl[14] = {
      (const void*)&dxs_QI,             // 0
      (const void*)&dxs_AR,             // 1
      (const void*)&dxs_Rel,            // 2
      (const void*)&dxs_GetDevice,      // 3
      (const void*)&dxs_GetTransform,   // 4
      (const void*)&dxs_SetTransform,   // 5
      (const void*)&dxs_SetWorldViewRH, // 6
      (const void*)&dxs_SetWorldViewLH, // 7
      (const void*)&dxs_Begin,          // 8
      (const void*)&dxs_Draw,           // 9
      (const void*)&dxs_Flush,          // 10
      (const void*)&dxs_End,            // 11
      (const void*)&dxs_OnLostDevice,   // 12
      (const void*)&dxs_OnResetDevice,  // 13
  };
  sprite_obj* s = (sprite_obj*)malloc(sizeof(sprite_obj));
  s->vtbl = k_sprite_vtbl;
  s->ref = 1;
  *out_sprite = s;
  char line[64];
  snprintf(line, sizeof(line), "d3dx9: CreateSprite");
  log_line(line);
  return 0;
}

int trt_bridge_register_d3dx9(void) {
  static const trt_named_fn k_fns[] = {
      {"D3DXMatrixIdentity", (void*)&dx_MatrixIdentity},
      {"D3DXMatrixTranslation", (void*)&dx_MatrixTranslate},
      {"D3DXMatrixScaling", (void*)&dx_MatrixScale},
      {"D3DXMatrixRotationX", (void*)&dx_MatrixRotateX},
      {"D3DXMatrixRotationY", (void*)&dx_MatrixRotateY},
      {"D3DXMatrixRotationZ", (void*)&dx_MatrixRotateZ},
      {"D3DXMatrixMultiply", (void*)&dx_MatrixMultiply},
      {"D3DXMatrixPerspectiveFovLH", (void*)&dx_MatrixPerspectiveFovLH},
      {"D3DXMatrixPerspectiveFovRH", (void*)&dx_MatrixPerspectiveFovRH},
      {"D3DXMatrixOrthoLH", (void*)&dx_MatrixOrthoLH},
      {"D3DXMatrixOrthoOffCenterLH", (void*)&dx_MatrixOrthoOffCenterLH},
      {"D3DXMatrixLookAtLH", (void*)&dx_MatrixLookAtLH},
      {"D3DXMatrixInverse", (void*)&dx_MatrixInverse},
      {"D3DXMatrixTranspose", (void*)&dx_MatrixTranspose},
      {"D3DXMatrixAffineTransformation",
       (void*)&dx_MatrixAffineTransformation},
      {"D3DXVec4Transform", (void*)&dx_Vec4Transform},
      {"D3DXVec3Normalize", (void*)&dx_Vec3Normalize},
      {"D3DXVec3Cross", (void*)&dx_Vec3Cross},
      {"D3DXVec3Dot", (void*)&dx_Vec3Dot},
      {"D3DXVec3Length", (void*)&dx_Vec3Length},
      {"D3DXQuaternionRotationMatrix",
       (void*)&dx_QuatRotationMatrix},
      {"D3DXQuaternionSlerp", (void*)&dx_QuatSlerp},
      {"D3DXCreateTextureFromFileInMemory",
       (void*)&dx_CreateTextureFromFileInMemory},
      {"D3DXCreateTexture", (void*)&dx_CreateTexture},
      {"D3DXAssembleShader", (void*)&dx_AssembleShader},
      {"D3DXCreateSprite", (void*)&dx_CreateSprite},
  };
  trt_module_register("d3dx9_43.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("d3dx9_40.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("d3dx9_39.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("d3dx9_38.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("d3dx9_37.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("d3dx9_36.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("d3dx9_35.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("d3dx9_34.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("d3dx9_33.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("d3dx9_32.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("d3dx9_31.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("d3dx9_30.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
