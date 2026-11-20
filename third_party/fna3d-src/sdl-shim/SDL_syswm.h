/* sdl-shim/SDL_syswm.h — minimal SDL_syswm.h for the UWP build.
 *
 * Only the Windows part of SDL_SysWMinfo is provided: FNA3D's D3D11
 * driver reads info.info.win.window to get the HWND for swapchain
 * creation on the stock path. On the console there is no SDL window, so
 * SDL_GetWindowWMInfo always returns SDL_FALSE (honest failure); the
 * host-adopted path never calls it.
 */

#ifndef TIRAMISU_SDL_SHIM_SYSWM_H
#define TIRAMISU_SDL_SHIM_SYSWM_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SDL_SYSWM_TYPE
{
	SDL_SYSWM_UNKNOWN = 0,
	SDL_SYSWM_WINDOWS
} SDL_SYSWM_TYPE;

struct SDL_SysWMinfo
{
	SDL_version version;
	SDL_SYSWM_TYPE subsystem;
	union
	{
		struct
		{
			HWND window;
			HDC hdc;
			HINSTANCE hinstance;
		} win;
		/* Other platforms do not exist in this shim. */
		int dummy;
	} info;
};
typedef struct SDL_SysWMinfo SDL_SysWMinfo;

SDL_bool SDL_GetWindowWMInfo(SDL_Window *window, SDL_SysWMinfo *info);

#ifdef __cplusplus
}
#endif

#endif /* TIRAMISU_SDL_SHIM_SYSWM_H */
