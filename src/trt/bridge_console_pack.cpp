// tiramisu/src/trt/bridge_console_pack.cpp
// Umbrella registration for the console-missing desktop DLL bridges.
#include "bridge_console_pack.h"
#include "bridge_opengl32.h"
#include "bridge_hid.h"
#include "bridge_xinput.h"
#include "bridge_wasapi.h"
#include "bridge_winrt.h"
#include "bridge_ws2.h"
#include "bridge_msvcrt.h"

extern "C" int trt_bridge_register_console_pack(void) {
  trt_bridge_register_opengl32();
  trt_bridge_register_hid();
  trt_bridge_register_xinput();
  trt_bridge_register_wasapi();
  trt_bridge_register_winrt();
  trt_bridge_register_ws2();
  trt_bridge_register_msvcrt();
  return 0;
}
