// tiramisu/src/trt/bridge_console_pack.h
// Registration umbrella for console-missing desktop DLL bridges:
//   - opengl32.dll (WGL + GL 1.1 fail-visible bridge)
//   - hid.dll + setupapi.dll (empty HID device list -> XInput fallback)
//   - xinput1_4 / xinput1_3 / xinput9_1_0 / xinputuap (pad state + ordinals)
//   - mmdevapi / AudioClient WASAPI COM stack + avrt.dll
//   - combase.dll (WinRT Ro* + HSTRING) + dwrite.dll + uiautomationcore.dll
//     + shcore.dll + version.dll + winhttp.dll
#ifndef TIRAMISU_BRIDGE_CONSOLE_PACK_H
#define TIRAMISU_BRIDGE_CONSOLE_PACK_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_console_pack(void);

#ifdef __cplusplus
}
#endif

#endif
