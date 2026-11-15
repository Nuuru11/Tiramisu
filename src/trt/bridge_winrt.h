// tiramisu/src/trt/bridge_winrt.h
// combase.dll (WinRT Ro* / HSTRING / COM base), dwrite.dll,
// uiautomationcore.dll, shcore.dll, version.dll, winhttp.dll BUILTIN bridge.
#ifndef TIRAMISU_BRIDGE_WINRT_H
#define TIRAMISU_BRIDGE_WINRT_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_winrt(void);

#ifdef __cplusplus
}
#endif

#endif
