// tiramisu/src/trt/pe_exports.h
// Export directory lookup: how a guest resolves a symbol from a DLL it
// loaded itself (GetProcAddress) and how one guest DLL resolves another.
#ifndef TIRAMISU_PE_EXPORTS_H
#define TIRAMISU_PE_EXPORTS_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Find an exported symbol in a mapped image.
//   name    - symbol name, or NULL to look up by ordinal
//   ordinal - ordinal to look up when name is NULL
// Returns the callable address, or NULL if the image does not export it.
// Forwarded exports ("KERNEL32.Sleep") are resolved through `rs`.
void* trt_pe_find_export(const trt_backend* be, const trt_resolver* rs,
                         const trt_image* img, const char* name, u16 ordinal);

// Does the image export this name at all (cheap check)?
int trt_pe_has_export(const trt_image* img, const char* name);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_PE_EXPORTS_H
