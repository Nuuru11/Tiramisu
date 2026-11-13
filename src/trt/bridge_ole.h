// tiramisu/src/trt/bridge_ole.h
// Minimal COM boot + OLEAUT for the guest (bridge_ole.cpp).
//
// What games need at boot: CoInitializeEx (apartment token), CoUninitialize,
// CoCreateInstance (fail-visible for classes we don't provide; CLSID personalities
// we DO provide are registered here), CoTaskMemAlloc/Free, and the OLEAUT
// memory pair (SysAllocString/SysFreeString/SysStringLen) plus VariantInit/
// Clear and SafeArrayCreate/Destroy — DirectShow/DSound/COM-heavy engines
// probe these before any real COM call.
//
// NOT a full COM: aggregation, marshaling, and apartment isolation are out
// of scope by evidence (single guest thread drives everything).
#ifndef TIRAMISU_BRIDGE_OLE_H
#define TIRAMISU_BRIDGE_OLE_H

#include "trt.h"

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

#ifdef __cplusplus
}
#endif

#endif
