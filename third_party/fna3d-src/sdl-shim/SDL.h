/* sdl-shim/SDL.h — minimal SDL2-compatibility shim for the UWP build.
 *
 * Tiramisu links NO SDL on the console: there is no SDL window, no SDL
 * video, no SDL audio in the UWP host. But FNA3D's D3D11 driver (and
 * FNA3D.c) are written against SDL2 for memory, mutexes, dynamic loading,
 * hints, logging and window bookkeeping. This shim provides exactly the
 * SDL2 surface those two translation units use, implemented on Win32/UWP
 * primitives that exist in the AppContainer partition.
 *
 * What is REAL here (full implementations, not stubs):
 *   - memory/string (SDL_malloc/free/realloc, memcpy/memset/..., strlen,
 *     strcmp, snprintf/vsnprintf): the C runtime, via macros.
 *   - mutexes (SDL_CreateMutex/...): CRITICAL_SECTION.
 *   - dynamic loading (SDL_LoadObject/LoadFunction): GetModuleHandleA /
 *     GetProcAddress on modules the host already links (d3d11.dll,
 *     dxgi.dll). SDL_UnloadObject is a no-op because nothing is loaded
 *     by us -- freeing a GetModuleHandle handle would be wrong.
 *   - hints (SDL_GetHint/SetHint/GetHintBoolean): a small real key/value
 *     store (16 slots). Unset hints return NULL / the caller's default.
 *   - logging (SDL_LogInfo/Warn/Error): OutputDebugStringA.
 *   - iconv (SDL_iconv_open/iconv/close): real UTF-8 -> WCHAR_T via
 *     MultiByteToWideChar (the only conversion FNA3D uses, in
 *     D3D11_SetStringMarker). Any other conversion fails honestly.
 *   - SDL_strcasecmp: _stricmp.
 *
 * What is HONESTLY ABSENT (no SDL windows exist on the console):
 *   - SDL_GetWindowData/SDL_SetWindowData: no storage, always NULL.
 *   - SDL_GetWindowSizeInPixels: reports 0x0 (no SDL window to measure).
 *   - SDL_GetWindowWMInfo (SDL_syswm.h): returns SDL_FALSE.
 *   The host-adopted D3D11 path never calls these (it gets the drawable
 *   size from the host swapchain instead); they exist so the stock path
 *   still compiles, and fail visibly if ever reached.
 *
 * Version claimed: SDL 2.26.0 -- the minimum FNA3D requires
 * (FNA3D.c errors below 2.26). The claim is about the API surface this
 * shim provides, not about real SDL being present; see README.md.
 *
 * This shim is Tiramisu-only and is NEVER on the include path of the
 * Linux build (which uses real SDL2 via CMake).
 */

#ifndef TIRAMISU_SDL_SHIM_H
#define TIRAMISU_SDL_SHIM_H

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WINDOWS_
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Version (2.26.0: the minimum FNA3D accepts) */

#define SDL_MAJOR_VERSION 2
#define SDL_MINOR_VERSION 26
#define SDL_PATCHLEVEL 0

typedef struct SDL_version
{
	uint8_t major;
	uint8_t minor;
	uint8_t patch;
} SDL_version;

#define SDL_VERSION(x) \
	do { \
		(x)->major = SDL_MAJOR_VERSION; \
		(x)->minor = SDL_MINOR_VERSION; \
		(x)->patch = SDL_PATCHLEVEL; \
	} while (0)

#define SDL_VERSION_ATLEAST(X, Y, Z) \
	((SDL_MAJOR_VERSION * 10000 + SDL_MINOR_VERSION * 100 + SDL_PATCHLEVEL) >= \
	 ((X) * 10000 + (Y) * 100 + (Z)))

/* Basic types */

typedef int SDL_bool;
#define SDL_FALSE 0
#define SDL_TRUE 1

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef int64_t Sint64;
typedef uint64_t Uint64;

/* Memory / string: the C runtime */

#define SDL_malloc malloc
#define SDL_free free
#define SDL_realloc realloc
#define SDL_memcpy memcpy
#define SDL_memmove memmove
#define SDL_memset memset
#define SDL_memcmp memcmp
#define SDL_strlen strlen
#define SDL_strcmp strcmp
#define SDL_strncmp strncmp
#define SDL_strchr strchr
#define SDL_strstr strstr
#define SDL_snprintf snprintf
#define SDL_vsnprintf vsnprintf
#define SDL_assert assert

#define SDL_zerop(x) SDL_memset((x), 0, sizeof(*(x)))
#define SDL_zero(x) SDL_memset(&(x), 0, sizeof(x))
#define SDL_arraysize(array) (sizeof(array) / sizeof((array)[0]))
#define SDL_max(a, b) (((a) > (b)) ? (a) : (b))
#define SDL_min(a, b) (((a) < (b)) ? (a) : (b))

int SDL_strcasecmp(const char *a, const char *b);

/* Mutexes: CRITICAL_SECTION */

typedef struct SDL_mutex
{
	CRITICAL_SECTION cs;
} SDL_mutex;

SDL_mutex *SDL_CreateMutex(void);
void SDL_DestroyMutex(SDL_mutex *mutex);
int SDL_LockMutex(SDL_mutex *mutex);
int SDL_UnlockMutex(SDL_mutex *mutex);

/* Dynamic loading: resolve from already-loaded modules only */

void *SDL_LoadObject(const char *sofile);
void *SDL_LoadFunction(void *handle, const char *name);
void SDL_UnloadObject(void *handle);

/* Hints: a small real key/value store */

#define SDL_HINT_VIDEO_EXTERNAL_CONTEXT "SDL_VIDEO_EXTERNAL_CONTEXT"

const char *SDL_GetHint(const char *name);
SDL_bool SDL_SetHint(const char *name, const char *value);
SDL_bool SDL_GetHintBoolean(const char *name, SDL_bool default_value);

/* Windows: opaque; there are no SDL windows on the console */

typedef struct SDL_Window SDL_Window;

void *SDL_GetWindowData(SDL_Window *window, const char *name);
void *SDL_SetWindowData(SDL_Window *window, const char *name, void *userdata);
void SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h);

/* Iconv: real UTF-8 -> WCHAR_T only */

typedef void *SDL_iconv_t;
#define SDL_ICONV_ERROR ((size_t) -1)
#define SDL_ICONV_E2BIG ((size_t) -2)
#define SDL_ICONV_EILSEQ ((size_t) -3)
#define SDL_ICONV_EINVAL ((size_t) -4)

SDL_iconv_t SDL_iconv_open(const char *tocode, const char *fromcode);
int SDL_iconv_close(SDL_iconv_t cd);
size_t SDL_iconv(SDL_iconv_t cd, const char **inbuf, size_t *inbytesleft,
		 char **outbuf, size_t *outbytesleft);

/* Logging: OutputDebugStringA */

#define SDL_LOG_CATEGORY_APPLICATION 0

void SDL_LogInfo(int category, const char *fmt, ...);
void SDL_LogWarn(int category, const char *fmt, ...);
void SDL_LogError(int category, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* TIRAMISU_SDL_SHIM_H */
