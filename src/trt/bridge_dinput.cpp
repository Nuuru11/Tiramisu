// tiramisu/src/trt/bridge_dinput.cpp
// DirectInput 8 for the guest.
//
// Most PC games read the mouse and keyboard through DirectInput 8, not
// through Win32 messages. Without it a game boots and gets NO input at
// all - so this bridge is what makes the mouse actually move on screen.
//
// Two COM objects with the SDK's exact vtables:
//   IDirectInput8W          3 + 8  slots
//   IDirectInputDevice8W    3 + 31 slots
// The device hands out the classic states: DIMOUSESTATE (dx/dy/buttons)
// and the 256-byte keyboard state (one byte per virtual key).
#include "bridge_dinput.h"
#include "thunk.h"
#include "input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int E_NOTIMPL_ = 0x80004001;
constexpr int DI_OK_ = 0;

// the keyboard and mouse GUIDs (dinput.h)
// GUID_SysKeyboard {6F1D2B61-D5A0-11CF-BFC7-444553540000} (in-memory GUID)
const u8 k_guid_keyboard[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF,
                                0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54,
                                0x00, 0x00};
// GUID_SysMouse {6F1D2B60-D5A0-11CF-BFC7-444553540000} (in-memory GUID)
const u8 k_guid_mouse[16] = {0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                             0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};

enum device_kind { DEV_KEYBOARD = 1, DEV_MOUSE = 2 };

struct dinput_device {
  const void** vtbl;
  u32 refcount;
  u32 kind;         // device_kind
  u32 acquired;
  u32 coop_level;
};

struct dinput_object {
  const void** vtbl;
  u32 refcount;
};

// DIMOUSESTATE: LONG lX, lY, lZ + BYTE rgbButtons[8] (0x80 = down)
struct mouse_state {
  i32 lX;
  i32 lY;
  i32 lZ;
  u8 rgbButtons[8];
};

// the objects and vtables are defined further down
extern dinput_device g_dev_keyboard;
extern dinput_device g_dev_mouse;
extern const void* g_di8_vtbl[11];
extern const void* g_didev_vtbl[34];


// the objects and the vtables are defined below

// ---- IUnknown (shared by both objects) ----
int TRT_GUEST_ABI di_QueryInterface(void* self, const u8* riid, void** out) {
  (void)riid;
  if (out) *out = self;
  return DI_OK_;
}
u32 TRT_GUEST_ABI di_AddRef(void* self) {
  u32* r = (u32*)self;   // refcount is the second field of both objects
  return ++r[1];
}
u32 TRT_GUEST_ABI di_Release(void* self) {
  u32* r = (u32*)self;
  return r[1] ? --r[1] : 0;
}

// ---- IDirectInput8 ----
int TRT_GUEST_ABI di_CreateDevice(void* self, const u8* guid, void** out_device,
                                  void* unk) {
  (void)self; (void)unk;
  if (!out_device) return E_NOTIMPL_;
  if (guid && memcmp(guid, k_guid_mouse, 16) == 0) {
    *out_device = &g_dev_mouse;
  } else if (guid && memcmp(guid, k_guid_keyboard, 16) == 0) {
    *out_device = &g_dev_keyboard;
  } else {
    // an unknown GUID: hand back the mouse (the game usually asks for a
    // keyboard or a mouse by GUID, but some ask for a "system" device)
    *out_device = &g_dev_mouse;
  }
  return DI_OK_;
}

int TRT_GUEST_ABI di_EnumDevices(void* self, u32 type, void* callback,
                                 void* context, u32 flags) {
  (void)self; (void)type; (void)flags;
  if (!callback) return E_NOTIMPL_;
  // the callback gets (LPDIDEVICEINSTANCEW, LPVOID)
  using cb_t = int (*)(const void*, void*);
  // a minimal DIDEVICEINSTANCEW: guid, ts, name, ...
  struct {
    u32 size;
    u32 guid;
    u32 product;
    u32 reserved;
    u16 align;
    u16 version;
    u8 ts[16];
    u16 name[32];
    u16 instance[32];
  } dev;
  memset(&dev, 0, sizeof(dev));
  dev.size = sizeof(dev);
  ((cb_t)callback)(&dev, context);
  return DI_OK_;
}

int TRT_GUEST_ABI di_GetDeviceStatus(void* self, const u8* guid) {
  (void)self; (void)guid;
  return DI_OK_;  // DISCONNECTED == 0, so say "connected"
}
int TRT_GUEST_ABI di_RunControlPanel(void*, void*, u32) { return E_NOTIMPL_; }
int TRT_GUEST_ABI di_Initialize(void*, void*, u32) { return DI_OK_; }
int TRT_GUEST_ABI di_FindDevice(void*, const u8*, const u16*, u8** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI di_EnumDevicesBySemantics(void*, const u16*, void*, void*,
                                            void*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI di_ConfigureDevices(void*, void*, void*, u32, void*) {
  return E_NOTIMPL_;
}

// ---- IDirectInputDevice8 ----
int TRT_GUEST_ABI dd_GetCapabilities(void* self, void* caps) {
  dinput_device* d = (dinput_device*)self;
  if (!caps) return E_NOTIMPL_;
  memset(caps, 0, 220);
  u8* c = (u8*)caps;
  c[0] = 220;  // dwSize
  // dwDevType (offset 4): keyboard = 4, mouse = 3
  c[4] = (u8)(d->kind == DEV_KEYBOARD ? 4 : 3);
  return DI_OK_;
}
int TRT_GUEST_ABI dd_EnumObjects(void*, void*, void*, u32) { return E_NOTIMPL_; }
int TRT_GUEST_ABI dd_GetProperty(void*, const u8*, void*) { return E_NOTIMPL_; }
int TRT_GUEST_ABI dd_SetProperty(void*, const u8*, const void*) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_Acquire(void* self) {
  ((dinput_device*)self)->acquired = 1;
  return DI_OK_;
}
int TRT_GUEST_ABI dd_Unacquire(void* self) {
  ((dinput_device*)self)->acquired = 0;
  return DI_OK_;
}

// GetDeviceState: the state a game actually polls
int TRT_GUEST_ABI dd_GetDeviceState(void* self, u32 size, void* buf) {
  dinput_device* d = (dinput_device*)self;
  if (!buf) return E_NOTIMPL_;
  if (d->kind == DEV_MOUSE) {
    if (size < sizeof(mouse_state)) return E_NOTIMPL_;
    mouse_state st;
    int dx = 0, dy = 0;
    trt_mouse_delta(&dx, &dy);           // consumes the accumulated motion
    st.lX = dx;
    st.lY = dy;
    st.lZ = trt_mouse_wheel();           // consumes
    memset(st.rgbButtons, 0, sizeof(st.rgbButtons));
    if (trt_mouse_button(0)) st.rgbButtons[0] = 0x80;
    if (trt_mouse_button(1)) st.rgbButtons[1] = 0x80;
    if (trt_mouse_button(2)) st.rgbButtons[2] = 0x80;
    memcpy(buf, &st, sizeof(st));
    return DI_OK_;
  }
  // keyboard: one byte per virtual key, 0x80 = down
  if (size < 256) return E_NOTIMPL_;
  u8 keys[256];
  memset(keys, 0, sizeof(keys));
  for (int vk = 0; vk < 256; ++vk)
    if (trt_key_down((u16)vk)) keys[vk] = 0x80;
  memcpy(buf, keys, sizeof(keys));
  return DI_OK_;
}

int TRT_GUEST_ABI dd_GetDeviceData(void*, u32, void*, u32*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_SetDataFormat(void*, const void*) { return DI_OK_; }
int TRT_GUEST_ABI dd_SetEventNotification(void*, void*) { return DI_OK_; }
int TRT_GUEST_ABI dd_SetCooperativeLevel(void* self, void* hwnd, u32 level) {
  ((dinput_device*)self)->coop_level = level;
  return DI_OK_;
}
int TRT_GUEST_ABI dd_GetObjectInfo(void*, void*, u32, u32) { return E_NOTIMPL_; }
int TRT_GUEST_ABI dd_GetDeviceInfo(void*, void*) { return E_NOTIMPL_; }
int TRT_GUEST_ABI dd_RunControlPanel(void*, void*, u32) { return E_NOTIMPL_; }
int TRT_GUEST_ABI dd_Initialize(void*, void*, u32, const u8*) { return DI_OK_; }
int TRT_GUEST_ABI dd_CreateEffect(void*, const u8*, const void*, void** out,
                                  void*) {
  if (out) *out = nullptr;
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_EnumEffects(void*, void*, void*, u32) { return E_NOTIMPL_; }
int TRT_GUEST_ABI dd_GetEffectInfo(void*, void*, const u8*) { return E_NOTIMPL_; }
int TRT_GUEST_ABI dd_GetForceFeedbackState(void*, u32* state) {
  if (state) *state = 0;  // no force feedback on the console
  return DI_OK_;
}
int TRT_GUEST_ABI dd_SendForceFeedbackCommand(void*, u32) { return DI_OK_; }
int TRT_GUEST_ABI dd_EnumCreatedEffectObjects(void*, void*, void*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_Escape(void*, void*) { return E_NOTIMPL_; }
int TRT_GUEST_ABI dd_Poll(void*) { return DI_OK_; }
int TRT_GUEST_ABI dd_SendDeviceData(void*, u32, const void*, u32*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_EnumEffectsInFile(void*, const u16*, void*, void*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_WriteEffectToFile(void*, const u16*, u32, void*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_BuildActionMap(void*, void*, const u16*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_SetActionMap(void*, void*, const u16*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_GetImageInfo(void*, void*) { return E_NOTIMPL_; }


dinput_object g_di8_obj = {g_di8_vtbl, 1};
dinput_device g_dev_keyboard = {g_didev_vtbl, 1, DEV_KEYBOARD, 0, 0};
dinput_device g_dev_mouse = {g_didev_vtbl, 1, DEV_MOUSE, 0, 0};

// the objects and the vtables are defined below
extern const void* g_di8_vtbl[11];
extern const void* g_didev_vtbl[34];

// IUnknown (defined below, shared by both objects)
int di_QueryInterface(void* self, const u8* riid, void** out);
u32 di_AddRef(void* self);
u32 di_Release(void* self);

// ---- IDirectInput8W (slots 3..10) ----
int di_CreateDevice(void* self, const u8* guid, void** out_device,
                    void* unk);
int di_EnumDevices(void* self, u32 type, void* callback, void* context,
                   u32 flags);
int di_GetDeviceStatus(void* self, const u8* guid);
int di_RunControlPanel(void* self, void* hwnd, u32 flags);
int di_Initialize(void* self, void* hinst, u32 version);
int di_FindDevice(void* self, const u8* guid, const u16* name, u8** out);
int di_EnumDevicesBySemantics(void* self, const u16* semantics, void* fmt,
                              void* callback, void* context, u32 flags);
int di_ConfigureDevices(void* self, void* callback, void* params, u32 flags,
                        void* context);

// IUnknown + the eight IDirectInput8 methods
const void* g_di8_vtbl[11] = {
    (const void*)&di_QueryInterface,
    (const void*)&di_AddRef,
    (const void*)&di_Release,
    (const void*)&di_CreateDevice,
    (const void*)&di_EnumDevices,
    (const void*)&di_GetDeviceStatus,
    (const void*)&di_RunControlPanel,
    (const void*)&di_Initialize,
    (const void*)&di_FindDevice,
    (const void*)&di_EnumDevicesBySemantics,
    (const void*)&di_ConfigureDevices,
};

// ---- IDirectInputDevice8W (slots 3..33) ----
int dd_GetCapabilities(void* self, void* caps);
int dd_EnumObjects(void* self, void* callback, void* context, u32 flags);
int dd_GetProperty(void* self, const u8* guid, void* header);
int dd_SetProperty(void* self, const u8* guid, const void* header);
int dd_Acquire(void* self);
int dd_Unacquire(void* self);
int dd_GetDeviceState(void* self, u32 size, void* buf);
int dd_GetDeviceData(void* self, u32 size, void* data, u32* count, u32 flags);
int dd_SetDataFormat(void* self, const void* format);
int dd_SetEventNotification(void* self, void* handle);
int dd_SetCooperativeLevel(void* self, void* hwnd, u32 level);
int dd_GetObjectInfo(void* self, void* info, u32 size, u32 flags);
int dd_GetDeviceInfo(void* self, void* info);
int dd_RunControlPanel(void* self, void* hwnd, u32 flags);
int dd_Initialize(void* self, void* hinst, u32 version, const u8* guid);
int dd_CreateEffect(void* self, const u8* guid, const void* desc,
                    void** out, void* unk);
int dd_EnumEffects(void* self, void* callback, void* context, u32 flags);
int dd_GetEffectInfo(void* self, void* info, const u8* guid);
int dd_GetForceFeedbackState(void* self, u32* state);
int dd_SendForceFeedbackCommand(void* self, u32 cmd);
int dd_EnumCreatedEffectObjects(void* self, void* callback, void* context,
                                u32 flags);
int dd_Escape(void* self, void* escape);
int dd_Poll(void* self);
int dd_SendDeviceData(void* self, u32 size, const void* data, u32* count,
                      u32 flags);
int dd_EnumEffectsInFile(void* self, const u16* file, void* callback,
                         void* context, u32 flags);
int dd_WriteEffectToFile(void* self, const u16* file, u32 size, void* data,
                         u32 flags);
int dd_BuildActionMap(void* self, void* format, const u16* path, u32 flags);
int dd_SetActionMap(void* self, void* format, const u16* path, u32 flags);
int dd_GetImageInfo(void* self, void* info);

const void* g_didev_vtbl[34] = {
    (const void*)&di_QueryInterface,
    (const void*)&di_AddRef,
    (const void*)&di_Release,
    (const void*)&dd_GetCapabilities,
    (const void*)&dd_EnumObjects,
    (const void*)&dd_GetProperty,
    (const void*)&dd_SetProperty,
    (const void*)&dd_Acquire,
    (const void*)&dd_Unacquire,
    (const void*)&dd_GetDeviceState,
    (const void*)&dd_GetDeviceData,
    (const void*)&dd_SetDataFormat,
    (const void*)&dd_SetEventNotification,
    (const void*)&dd_SetCooperativeLevel,
    (const void*)&dd_GetObjectInfo,
    (const void*)&dd_GetDeviceInfo,
    (const void*)&dd_RunControlPanel,
    (const void*)&dd_Initialize,
    (const void*)&dd_CreateEffect,
    (const void*)&dd_EnumEffects,
    (const void*)&dd_GetEffectInfo,
    (const void*)&dd_GetForceFeedbackState,
    (const void*)&dd_SendForceFeedbackCommand,
    (const void*)&dd_EnumCreatedEffectObjects,
    (const void*)&dd_Escape,
    (const void*)&dd_Poll,
    (const void*)&dd_SendDeviceData,
    (const void*)&dd_EnumEffectsInFile,
    (const void*)&dd_WriteEffectToFile,
    (const void*)&dd_BuildActionMap,
    (const void*)&dd_SetActionMap,
    (const void*)&dd_GetImageInfo,
};

} // namespace

extern "C" {

// ---- guest-side input feed (guest ABI: the guest calls these directly) ----
void TRT_GUEST_ABI trt_di_feed_move(i32 dx, i32 dy) {
  trt_input_mouse_delta_raw(dx, dy);
}
void TRT_GUEST_ABI trt_di_feed_button(i32 btn, u32 down) {
  trt_input_mouse_button((int)btn, down != 0);
}
void TRT_GUEST_ABI trt_di_feed_key(u16 vk, u32 down) {
  trt_input_key_event(vk, down != 0);
}

// DirectInput8Create(hinst, version, riid, ppv, unk)
int TRT_GUEST_ABI trt_DirectInput8Create(void* hinst, u32 version,
                                         const u8* riid, void** out,
                                         void* unk) {
  (void)hinst; (void)version; (void)riid; (void)unk;
  if (!out) return E_NOTIMPL_;
  *out = &g_di8_obj;
  return DI_OK_;
}

int trt_bridge_register_dinput(void) {
  static const trt_named_fn k_fns[] = {
      {"DirectInput8Create", (void*)&trt_DirectInput8Create},
      {"trt_di_feed_move", (void*)&trt_di_feed_move},
      {"trt_di_feed_button", (void*)&trt_di_feed_button},
      {"trt_di_feed_key", (void*)&trt_di_feed_key},
  };
  trt_module_register("dinput8.dll", TRT_ROUTE_BUILTIN, k_fns,
                       (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("dinput.dll", TRT_ROUTE_BUILTIN, k_fns,
                       (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

} // extern "C"
