// tiramisu/src/trt/mojo_hlsl.c
// Unity C11/C17 build of icculus/mojoshader (Zlib) with D3D9 SM1/2/3 -> HLSL
// translation enabled (profiles d3d + hlsl). Compiled as C, never C++.
#define MOJOSHADER_NO_VERSION_INCLUDE 1
#define MOJOSHADER_EFFECT_SUPPORT 0

#define SUPPORT_PROFILE_D3D 1
#define SUPPORT_PROFILE_HLSL 1
#define SUPPORT_PROFILE_BYTECODE 0
#define SUPPORT_PROFILE_GLSL 0
#define SUPPORT_PROFILE_GLSL120 0
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
#include "../../third_party/mojoshader/mojoshader.c"

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
