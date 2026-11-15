// tiramisu/src/trt/bridge_ole.h
// Minimal COM boot + OLEAUT for the guest (bridge_ole.cpp).
//
// What games need at boot: CoInitializeEx (apartment token), CoUninitialize,
// CoCreateInstance (fail-visible for classes we don't provide; CLSID personalities
// we DO provide are registered here), CoTaskMemAlloc/Free, and the OLEAUT
// memory pair (SysAllocString/SysFreeString/SysStringLen) plus VariantInit/
// Clear and SafeArrayCreate/Destroy — DirectShow/DSound/COM-heavy engines
// probe these before any real COM call.
#ifndef TIRAMISU_BRIDGE_OLE_H
#define TIRAMISU_BRIDGE_OLE_H

#include "trt.h"
#include "thunk.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register ole32.dll + oleaut32.dll as BUILTIN.
int trt_bridge_register_ole(void);

// A bridge can register a CLSID personality: when a guest asks
// CoCreateInstance for this CLSID, `factory` runs with the requested IID
// and fills the out pointer (returns S_OK) or E_NOINTERFACE.
typedef int (*trt_com_factory)(const void* iid, void** out, void* user);
void trt_ole_register_class(const void* clsid, trt_com_factory factory,
                            void* user);

// Shared COM entry points re-exported by combase.dll (bridge_winrt.cpp).
int TRT_GUEST_ABI ole_CoInitialize(void* reserved);
int TRT_GUEST_ABI ole_CoInitializeEx(void* reserved, u32 model);
void TRT_GUEST_ABI ole_CoUninitialize(void);
void TRT_GUEST_ABI ole_CoFreeUnusedLibraries(void);
int TRT_GUEST_ABI ole_CoCreateInstance(const void* clsid, void* outer,
                                       u32 context, const void* iid,
                                       void** out);
int TRT_GUEST_ABI ole_CoGetClassObject(const void* clsid, u32 context,
                                       void* reserved, const void* iid,
                                       void** out);
int TRT_GUEST_ABI ole_CoGetApartmentType(u32* apt_type, u32* qualifier);
void* TRT_GUEST_ABI ole_CoTaskMemAlloc(u32 bytes);
void* TRT_GUEST_ABI ole_CoTaskMemRealloc(void* p, u32 bytes);
void TRT_GUEST_ABI ole_CoTaskMemFree(void* p);
int TRT_GUEST_ABI ole_CoCreateFreeThreadedMarshaler(void* outer, void** out);
int TRT_GUEST_ABI ole_PropVariantClear(void* p);
int TRT_GUEST_ABI ole_RoGetAgileReference(u32 opts, const void* iid,
                                          void* unk, void** out);
int TRT_GUEST_ABI ole_CoWaitForMultipleHandles(u32 flags, u32 timeout_ms,
                                               u32 count,
                                               const void* handles,
                                               u32* index_out);
int TRT_GUEST_ABI ole_CoCreateGuid(void* out);
i32 TRT_GUEST_ABI ole_StringFromGUID2(const void* guid, u16* out, i32 cap);

#ifdef __cplusplus
}
#endif

#endif
