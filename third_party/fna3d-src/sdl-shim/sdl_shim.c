/* sdl-shim/sdl_shim.c — implementations for the SDL2-compatibility shim.
 * See SDL.h for what is real here vs. honestly absent. Compiled only in
 * the UWP build (tools/build-host.sh); never in the Linux build.
 *
 * All Win32 APIs used here (CRITICAL_SECTION, GetModuleHandleA,
 * GetProcAddress, MultiByteToWideChar, OutputDebugStringA) are in the
 * AppContainer partition.
 */

#include <SDL.h>
#include <SDL_syswm.h>

#include <limits.h>

/* strcasecmp */

int SDL_strcasecmp(const char *a, const char *b)
{
	return _stricmp(a, b);
}

/* Mutexes */

SDL_mutex *SDL_CreateMutex(void)
{
	SDL_mutex *mutex = (SDL_mutex*) SDL_malloc(sizeof(SDL_mutex));
	if (mutex == NULL)
	{
		return NULL;
	}
	InitializeCriticalSection(&mutex->cs);
	return mutex;
}

void SDL_DestroyMutex(SDL_mutex *mutex)
{
	if (mutex == NULL)
	{
		return;
	}
	DeleteCriticalSection(&mutex->cs);
	SDL_free(mutex);
}

int SDL_LockMutex(SDL_mutex *mutex)
{
	if (mutex == NULL)
	{
		return -1;
	}
	EnterCriticalSection(&mutex->cs);
	return 0;
}

int SDL_UnlockMutex(SDL_mutex *mutex)
{
	if (mutex == NULL)
	{
		return -1;
	}
	LeaveCriticalSection(&mutex->cs);
	return 0;
}

/* Dynamic loading: resolve from modules the host already links.
 * The host links d3d11.lib/dxgi.lib, so d3d11.dll/dxgi.dll are loaded in
 * the process and GetModuleHandleA finds them. Anything else returns
 * NULL (honest: LoadLibrary is not available in AppContainer).
 * SDL_UnloadObject is a no-op: we never load a module, and freeing a
 * GetModuleHandle handle would corrupt the host's own import.
 */

void *SDL_LoadObject(const char *sofile)
{
	if (sofile == NULL)
	{
		return NULL;
	}
	return (void*) GetModuleHandleA(sofile);
}

void *SDL_LoadFunction(void *handle, const char *name)
{
	if (handle == NULL || name == NULL)
	{
		return NULL;
	}
	return (void*) GetProcAddress((HMODULE) handle, name);
}

void SDL_UnloadObject(void *handle)
{
	(void) handle;
	/* No-op by design; see SDL.h. */
}

/* Hints: a small real key/value store (16 slots, not synchronized --
 * hints are read at device creation on one thread in FNA3D's use).
 */

#define SDL_SHIM_MAX_HINTS 16
#define SDL_SHIM_HINT_NAME_LEN 64
#define SDL_SHIM_HINT_VALUE_LEN 128

typedef struct SDL_ShimHint
{
	char name[SDL_SHIM_HINT_NAME_LEN];
	char value[SDL_SHIM_HINT_VALUE_LEN];
	int used;
} SDL_ShimHint;

static SDL_ShimHint g_hints[SDL_SHIM_MAX_HINTS];

const char *SDL_GetHint(const char *name)
{
	int i;
	if (name == NULL)
	{
		return NULL;
	}
	for (i = 0; i < SDL_SHIM_MAX_HINTS; i += 1)
	{
		if (g_hints[i].used && SDL_strcmp(g_hints[i].name, name) == 0)
		{
			return g_hints[i].value;
		}
	}
	return NULL;
}

SDL_bool SDL_SetHint(const char *name, const char *value)
{
	int i;
	int freeSlot = -1;
	if (name == NULL || value == NULL)
	{
		return SDL_FALSE;
	}
	for (i = 0; i < SDL_SHIM_MAX_HINTS; i += 1)
	{
		if (g_hints[i].used && SDL_strcmp(g_hints[i].name, name) == 0)
		{
			SDL_snprintf(g_hints[i].value, sizeof(g_hints[i].value), "%s", value);
			return SDL_TRUE;
		}
		if (!g_hints[i].used && freeSlot < 0)
		{
			freeSlot = i;
		}
	}
	if (freeSlot < 0)
	{
		return SDL_FALSE;
	}
	SDL_snprintf(g_hints[freeSlot].name, sizeof(g_hints[freeSlot].name), "%s", name);
	SDL_snprintf(g_hints[freeSlot].value, sizeof(g_hints[freeSlot].value), "%s", value);
	g_hints[freeSlot].used = 1;
	return SDL_TRUE;
}

SDL_bool SDL_GetHintBoolean(const char *name, SDL_bool default_value)
{
	const char *value = SDL_GetHint(name);
	if (value == NULL)
	{
		return default_value;
	}
	if (SDL_strcmp(value, "1") == 0 || SDL_strcasecmp(value, "true") == 0)
	{
		return SDL_TRUE;
	}
	if (SDL_strcmp(value, "0") == 0 || SDL_strcasecmp(value, "false") == 0)
	{
		return SDL_FALSE;
	}
	return default_value;
}

/* Windows: none exist on the console. Honest no-ops. */

void *SDL_GetWindowData(SDL_Window *window, const char *name)
{
	(void) window;
	(void) name;
	return NULL;
}

void *SDL_SetWindowData(SDL_Window *window, const char *name, void *userdata)
{
	(void) window;
	(void) name;
	(void) userdata;
	return NULL;
}

void SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h)
{
	(void) window;
	if (w != NULL)
	{
		*w = 0;
	}
	if (h != NULL)
	{
		*h = 0;
	}
}

SDL_bool SDL_GetWindowWMInfo(SDL_Window *window, SDL_SysWMinfo *info)
{
	(void) window;
	(void) info;
	return SDL_FALSE;
}

/* Iconv: real UTF-8 -> WCHAR_T only (what FNA3D uses). */

static int g_sdl_iconv_utf8_wchar_sentinel;

SDL_iconv_t SDL_iconv_open(const char *tocode, const char *fromcode)
{
	if (	tocode != NULL && fromcode != NULL &&
		SDL_strcasecmp(tocode, "WCHAR_T") == 0 &&
		SDL_strcasecmp(fromcode, "UTF-8") == 0	)
	{
		return (SDL_iconv_t) &g_sdl_iconv_utf8_wchar_sentinel;
	}
	return (SDL_iconv_t) -1;
}

int SDL_iconv_close(SDL_iconv_t cd)
{
	(void) cd;
	return 0;
}

size_t SDL_iconv(SDL_iconv_t cd, const char **inbuf, size_t *inbytesleft,
		 char **outbuf, size_t *outbytesleft)
{
	int inlen;
	int outwchars;
	int needed;
	int written;
	if (	cd != (SDL_iconv_t) &g_sdl_iconv_utf8_wchar_sentinel ||
		inbuf == NULL || *inbuf == NULL || inbytesleft == NULL ||
		outbuf == NULL || *outbuf == NULL || outbytesleft == NULL	)
	{
		return SDL_ICONV_ERROR;
	}
	if (*inbytesleft > (size_t) INT_MAX || *outbytesleft < sizeof(wchar_t))
	{
		return SDL_ICONV_ERROR;
	}
	inlen = (int) *inbytesleft;
	outwchars = (int) (*outbytesleft / sizeof(wchar_t));
	needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, *inbuf, inlen, NULL, 0);
	if (needed == 0)
	{
		return GetLastError() == ERROR_NO_UNICODE_TRANSLATION ?
			SDL_ICONV_EILSEQ : SDL_ICONV_ERROR;
	}
	if (needed > outwchars)
	{
		return SDL_ICONV_E2BIG;
	}
	written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, *inbuf, inlen,
				      (wchar_t*) *outbuf, outwchars);
	if (written == 0)
	{
		return GetLastError() == ERROR_NO_UNICODE_TRANSLATION ?
			SDL_ICONV_EILSEQ : SDL_ICONV_ERROR;
	}
	*inbuf += *inbytesleft;
	*inbytesleft = 0;
	*outbuf += (size_t) written * sizeof(wchar_t);
	*outbytesleft -= (size_t) written * sizeof(wchar_t);
	return 0;
}

/* Logging: OutputDebugStringA (visible in Dev Mode / debugger). */

static void SDL_ShimLog(const char *level, const char *fmt, va_list ap)
{
	char buf[1024];
	int prefix;
	prefix = SDL_snprintf(buf, sizeof(buf), "[FNA3D %s] ", level);
	if (prefix < 0 || prefix >= (int) sizeof(buf))
	{
		return;
	}
	SDL_vsnprintf(buf + prefix, sizeof(buf) - (size_t) prefix, fmt, ap);
	OutputDebugStringA(buf);
	OutputDebugStringA("\n");
}

void SDL_LogInfo(int category, const char *fmt, ...)
{
	va_list ap;
	(void) category;
	va_start(ap, fmt);
	SDL_ShimLog("info", fmt, ap);
	va_end(ap);
}

void SDL_LogWarn(int category, const char *fmt, ...)
{
	va_list ap;
	(void) category;
	va_start(ap, fmt);
	SDL_ShimLog("warn", fmt, ap);
	va_end(ap);
}

void SDL_LogError(int category, const char *fmt, ...)
{
	va_list ap;
	(void) category;
	va_start(ap, fmt);
	SDL_ShimLog("error", fmt, ap);
	va_end(ap);
}
