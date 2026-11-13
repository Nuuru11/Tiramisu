// tiramisu/src/trt/bridge_shell.h
// Shell for the guest (bridge_shell.cpp).
//
// What games use shell32 for at boot: SHGetFolderPath(CSIDL_MYDOCUMENTS /
// CSIDL_LOCAL_APPDATA / CSIDL_PROFILE) to find save-game and settings
// directories, SHGetKnownFolderPath (the newer GUID form), and
// ShellExecute(Ex) — which on console can only fail visibly (no shell, no
// browser, no associations). SHGetMalloc hands back a COM allocator that
// is just the CRT heap.
#ifndef TIRAMISU_BRIDGE_SHELL_H
#define TIRAMISU_BRIDGE_SHELL_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register shell32.dll (+ shfolder.dll alias) as BUILTIN.
int trt_bridge_register_shell(void);

#ifdef __cplusplus
}
#endif

#endif
