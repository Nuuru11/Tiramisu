// tiramisu/src/trt/images.h
// Guest image registry + on-demand loading of the DLLs a game ships.
//
// A shipped PC game is rarely one file: it brings engine DLLs, helper DLLs
// and often its own version.dll / winmm.dll shims. Without LoadLibrary the
// game cannot start, so the runtime loads guest DLLs the way Windows does:
// search the game's own directory, map, bind imports (recursively), seal,
// run DllMain, and hand out a module handle.
#ifndef TIRAMISU_IMAGES_H
#define TIRAMISU_IMAGES_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// The runtime context: backend, resolver, and where the game's own DLLs
// live on the host filesystem.
void trt_runtime_init(const trt_backend* be, const char* search_dir);
const trt_backend* trt_runtime_backend(void);
// The resolver that knows about guest images (and falls back to the module
// table for system modules). Use it for the exe AND for every guest DLL.
trt_resolver* trt_runtime_resolver(void);
// Where the game's own DLLs live on the host filesystem.
const char* trt_runtime_search_dir(void);
// The resolver itself (for callers that need to resolve a symbol).
void* trt_resolve_full(void* user, const char* module, const char* symbol,
                       u16 ordinal);

// ---- registry ----
trt_image* trt_images_find(const char* name);      // case-insensitive
trt_image* trt_images_by_handle(void* handle);
trt_image* trt_images_first(void);                  // first loaded guest image
// ---- enumeration (the psapi bridge exposes this as the REAL guest module
// list: K32EnumProcessModules / EnumProcesses / MiniDumpWriteDump module
// table all read from here) ----
int trt_images_count(void);
trt_image* trt_images_nth(int idx);
const char* trt_images_nth_name(int idx);   // "engine.dll"
const char* trt_images_nth_path(int idx);   // host path it was loaded from
void* trt_images_handle(trt_image* img);
const char* trt_images_name(void* handle);
// Register the main guest image (the exe mapped directly via trt_map_pe, not
// through trt_load_pe) so the psapi/dbghelp module list includes it.
void trt_images_register_main(trt_image* img, const char* guest_name);

// ---- loading ----
// Map a PE from a host path, register it, seal it. If `run_entry` is set,
// the entry point is called with (handle, 1, 0) - DllMain semantics.
trt_image* trt_load_pe(const char* host_path, const char* guest_name,
                       int run_entry);

// Call a guest function with the Win64 ABI, whatever the host ABI is.
// (On the console host this is a plain call; in the Linux test harness the
// argument registers differ, so we marshal them.)
u64 trt_call_guest(void* fn, u64 a, u64 b, u64 c, u64 d);

// Load-only mode toggle: 0 = map/bind/seal without running TLS callbacks or
// DllMain (corpus dry-run), 1 (default) = full boot semantics.
void trt_set_image_code_run(int enable);

// Drop every registered guest image (between corpus PEs: a stale image from
// a previous game directory must never satisfy the next game's imports).
void trt_images_reset(void);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_IMAGES_H
