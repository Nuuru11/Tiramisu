// tiramisu/src/trt/bridge_dbghelp.h
// dbghelp + dbgcore for the guest (bridge_dbghelp.cpp).
//
// SymInitialize -> S_OK with a real symbol-search record; SymFromAddr ->
// honest FALSE (no PDBs on console, named log). MiniDumpWriteDump writes a
// REAL TEXT crash report (exception address, thread id, the module list
// from the guest image registry) into the game dir through the VFS and
// returns TRUE with the dump path named in the log — a real diagnostic
// artifact replacing the ETW we do not have.
#ifndef TIRAMISU_BRIDGE_DBGHELP_H
#define TIRAMISU_BRIDGE_DBGHELP_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register dbghelp.dll + dbgcore.dll as BUILTIN.
int trt_bridge_register_dbghelp(void);

#ifdef __cplusplus
}
#endif

#endif
