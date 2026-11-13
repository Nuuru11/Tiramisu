// tiramisu/src/trt/bridge_reg.h
// A virtual registry for the guest (advapi32).
//
// UWP apps have no registry, but PC games read settings from HKLM/HKCU
// during startup; a game that gets nothing usually falls back to
// defaults, but one that expects a key may refuse to start. So the guest
// gets a real (small) registry: a key/value tree persisted next to the
// game's save data, with Wine-ish semantics.
#ifndef TIRAMISU_BRIDGE_REG_H
#define TIRAMISU_BRIDGE_REG_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mount the hive: the hive file lives on the guest's C: drive
// (C:\registry.hiv), so it sits with the game's saves.
void trt_registry_set_hive_path(const u16* guest_path);

// Register advapi32.dll's registry functions.
int trt_bridge_register_registry(void);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_BRIDGE_REG_H
