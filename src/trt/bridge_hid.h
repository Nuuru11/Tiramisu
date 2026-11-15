// tiramisu/src/trt/bridge_hid.h
// HID (hid.dll) + SetupAPI (setupapi.dll) device-enumeration bridge.
// Ported from ~/gow-uwp/ref/xboxnative/xbox-test/uwph/hid/backend.c.
//
// Xbox Series X|S UWP exposes gamepads via Windows.Gaming.Input / XInput
// (bridged in bridge_xinput.cpp), not raw desktop HID device nodes.
// Engines (Unity, Godot, SDL2) enumerate raw HID controllers at boot via
// HidD_GetHidGuid -> SetupDiGetClassDevsW -> SetupDiEnumDeviceInterfaces ->
// SetupDiDestroyDeviceInfoList, and fall back to XInput when the HID device
// list is empty (ERROR_NO_MORE_ITEMS = 259).
#ifndef TIRAMISU_BRIDGE_HID_H
#define TIRAMISU_BRIDGE_HID_H

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_hid(void);

#ifdef __cplusplus
}
#endif

#endif
