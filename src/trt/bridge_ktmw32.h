// tiramisu/src/trt/bridge_ktmw32.h
// Kernel Transaction Manager (ktmw32.dll) bridge. See bridge_ktmw32.cpp.
#ifndef TIRAMISU_BRIDGE_KTMW32_H
#define TIRAMISU_BRIDGE_KTMW32_H

#ifdef __cplusplus
extern "C" {
#endif

// Register ktmw32.dll as BUILTIN: transaction handles are real kernel32
// event handles (CloseHandle works), commit is truthful because VFS writes
// are applied immediately, rollback fails named (it cannot undo them).
int trt_bridge_register_ktmw32(void);

#ifdef __cplusplus
}
#endif

#endif
