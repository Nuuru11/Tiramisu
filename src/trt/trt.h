// tiramisu/src/trt/trt.h
// Tiramisu Runtime (trt) public API — Wine-style in-process PE64 loader
// for UWP hosts (Xbox Dev Mode) with a pluggable memory backend and
// import resolver so the same core runs in offline tests on Linux.
#ifndef TIRAMISU_TRT_H
#define TIRAMISU_TRT_H

#include "pe.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- protection bits (backend-translated) ----
#define TRT_PROT_R 1u
#define TRT_PROT_W 2u
#define TRT_PROT_X 4u

// ---- platform backend: what the loader needs from the host process ----
// UWP discipline: alloc returns RW memory, never RWX; exec pages exist only
// after seal flips them RX (VirtualProtectFromApp). Zero RWX, ever.
typedef struct trt_backend {
  // size in bytes, page-aligned internally; returns NULL on failure
  void* (*alloc)(size_t bytes, u64 preferred_base /*0 = anywhere*/);
  // change protection of [base, base+bytes); returns 0 on success
  int   (*protect)(void* base, size_t bytes, unsigned prot);
  void  (*release)(void* base, size_t bytes);
  void  (*log)(const char* line);           // line without trailing \n
} trt_backend;

// ---- import resolver: module+symbol -> callable address ----
// Return NULL to fail the load with TRT_ERR_IMPORT (name recorded).
typedef struct trt_resolver {
  void* user;
  void* (*resolve)(void* user, const char* module, const char* symbol,
                   u16 ordinal /*0 = by name*/);
} trt_resolver;

// ---- mapped image ----
#define TRT_MAX_TLS_CALLBACKS 64

typedef struct trt_image {
  u8*  base;              // actual load base
  u64  preferred_base;    // ImageBase from headers
  u64  size_of_image;
  u64  entry_rva;         // 0 = none
  u64  tls_callback_rvas[TRT_MAX_TLS_CALLBACKS]; // RVAs into the image
  int  tls_callback_count;
  u32  section_count;
  char last_detail[256];  // human-readable detail of last error/stage
} trt_image;

// Map a PE64 from a file image. Read-only view of [file, file+file_size).
// Stages: validate -> alloc -> headers+sections -> relocs -> imports -> TLS.
// Pages stay RW until trt_seal_image().
int trt_map_pe(const trt_backend* be, const trt_resolver* rs,
                const u8* file, size_t file_size, trt_image** out);

// Flip sections to final protections (code -> RX). Call once after
// relocation/import/TLS writes are complete. Returns TRT_OK or TRT_ERR_SEAL.
int trt_seal_image(const trt_backend* be, trt_image* img);

// free the image and its mapping
void trt_unmap(const trt_backend* be, trt_image* img);

const char* trt_errstr(int code);

// ---- module table: routing policy + builtin function registration ----
typedef enum {
  TRT_ROUTE_MISSING = 0, // module not registered: import from it fails
  TRT_ROUTE_SYSTEM = 1,  // console ships the real DLL; host binds it
  TRT_ROUTE_BUILTIN = 2, // Tiramisu implements it (function table below)
  TRT_ROUTE_STUB = 3     // exports exist, log when called, return 0
} trt_route;

typedef struct trt_named_fn {
  const char* name;
  void* fn;
} trt_named_fn;

void trt_module_register(const char* name, trt_route route,
                          const trt_named_fn* fns, int fn_count);
trt_route trt_module_route(const char* name);
void trt_resolver_default(trt_resolver* out);
typedef void* (*trt_system_load)(const char* module, const char* symbol);
void trt_set_system_loader(trt_system_load fn);
extern void trt_stub_thunk(void);

// bridge + vfs boot (declared for hosts/tests; defined in bridge_kernel32.cpp)
void trt_bridge_boot(const trt_backend* be);
int trt_bridge_register_kernel32(void);
void trt_bridge_new_thread_id(u32 id);
void trt_vfs_mount(const char* drive_prefix, const char* host_path);
int trt_vfs_resolve_utf16(const u16* guest_path, char* out, size_t out_cap);
void trt_bridge_set_exe_path(const char* zpath);
int trt_bridge_register_user32(void);
// host pump: called from the builtin GetMessageW while the guest blocks, so
// CoreWindow events + XInput keep flowing after the guest owns the thread
void trt_set_pump(void (*fn)(void));

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_TRT_H
