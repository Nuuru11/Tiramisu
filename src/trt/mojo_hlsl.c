// tiramisu/src/trt/mojo_hlsl.c
// Unity C11/C17 build of icculus/mojoshader (Zlib) with D3D9 SM1/2/3 -> HLSL
// translation enabled (profiles d3d + hlsl). Compiled as C, never C++.
//
// TIRAMISU MODIFICATION (Path B step 3): this unity build ALSO serves
// FNA3D's D3D11 driver, so the FNA3D sources must NOT compile MojoShader
// separately (duplicate symbols). Effect support is enabled (FNA3D needs
// the MOJOSHADER_effect API; it only ADDS preshader parsing -- non-effect
// translation is unchanged) and the effects + d3d11 backend files are
// included below. The d3d11 backend is Windows-only (it includes
// <d3d11.h>); on Linux the FNA3D D3D11 driver TU is empty, so the backend
// is unneeded there. NOTE: third_party/mojoshader/ and
// third_party/fna3d-src/MojoShader/ are identical copies (verified by
// diff) EXCEPT mojoshader_d3d11.c: its file-local static
// out_of_memory(void) was renamed d3d11_out_of_memory because the unity
// build also includes profile_common.c's global out_of_memory(Context*)
// and the two collide in one TU (separate-TU builds are unaffected, so
// the fna3d-src copy keeps the upstream name). The driver includes the
// fna3d-src header, this file builds the tiramisu copy -- same API, no
// mismatch.
#define MOJOSHADER_NO_VERSION_INCLUDE 1
#define MOJOSHADER_EFFECT_SUPPORT 1

#define SUPPORT_PROFILE_D3D 1
#define SUPPORT_PROFILE_HLSL 1
#define SUPPORT_PROFILE_BYTECODE 0
#ifndef _WIN32
#define SUPPORT_PROFILE_GLSL 1
#define SUPPORT_PROFILE_GLSL120 1
#else
#define SUPPORT_PROFILE_GLSL 0
#define SUPPORT_PROFILE_GLSL120 0
#endif
#define SUPPORT_PROFILE_GLSLES 0
#define SUPPORT_PROFILE_GLSLES3 0
#define SUPPORT_PROFILE_ARB1 0
#define SUPPORT_PROFILE_ARB1_NV 0
#define SUPPORT_PROFILE_METAL 0
#define SUPPORT_PROFILE_SPIRV 0
#define SUPPORT_PROFILE_GLSPIRV 0

#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include <malloc.h>
#include <string.h>
#ifndef alloca
#define alloca _alloca
#endif
#ifndef stricmp
#define stricmp _stricmp
#endif
#ifndef strnicmp
#define strnicmp _strnicmp
#endif
#else
#include <alloca.h>
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wdeprecated-non-prototype"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wshift-negative-value"
#endif

#include "../../third_party/mojoshader/mojoshader_common.c"
#include "../../third_party/mojoshader/profiles/mojoshader_profile_common.c"
#include "../../third_party/mojoshader/profiles/mojoshader_profile_d3d.c"
#include "../../third_party/mojoshader/profiles/mojoshader_profile_hlsl.c"
#ifndef _WIN32
#include "../../third_party/mojoshader/profiles/mojoshader_profile_glsl.c"
#endif
#include "../../third_party/mojoshader/mojoshader.c"
#include "../../third_party/mojoshader/mojoshader_effects.c"
#ifdef _WIN32
/* FNA3D D3D11 backend (MOJOSHADER_d3d11*). Windows-only: includes
 * <d3d11.h>. The WinRT branch (direct D3DCompile, no LoadLibrary) is
 * activated by the build's -DWINAPI_FAMILY_WINRT=1; see
 * tools/build-host.sh. */
#include "../../third_party/mojoshader/mojoshader_d3d11.c"
#endif

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
