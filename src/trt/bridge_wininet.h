// tiramisu/src/trt/bridge_wininet.h
// WinINet for the guest (bridge_wininet.cpp).
//
// WinINet is documented NOT available to UWP — a game importing wininet.dll
// would DIE AT LOAD on the TV if we let the console decide. This bridge
// binds every import and answers HONESTLY at call time:
//   InternetOpenW/A     -> a real HINTERNET handle record
//   HttpSendRequest*    -> FALSE + ERROR_INTERNET_CANNOT_CONNECT (no
//                          network stack for the guest on console) + log
//   InternetReadFile    -> TRUE with 0 bytes (clean EOF)
//   InternetCloseHandle -> frees the record
// Never silent-success with fake data.
#ifndef TIRAMISU_BRIDGE_WININET_H
#define TIRAMISU_BRIDGE_WININET_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register wininet.dll as BUILTIN.
int trt_bridge_register_wininet(void);

#ifdef __cplusplus
}
#endif

#endif
