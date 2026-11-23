// tiramisu/src/trt/universal_guest.h
// Phase 4: universal guest runner. Boots the universal guest PE — "the one
// engine for everything": Tiramisu boots any guest PE on Xbox. (Phase 3
// translates the PC game's imports into the universal guest's surface; this
// host path maps the guest image through the standard loader, the same path
// the console uses.)
#ifndef TIRAMISU_UNIVERSAL_GUEST_H
#define TIRAMISU_UNIVERSAL_GUEST_H

#include "trt.h"

// Load + map the universal guest PE. Returns the mapped image (caller
// trt_universal_guest_unload's it) or nullptr on failure.
trt_image* trt_universal_guest_load(const char* guest_exe,
                                    const trt_backend* be,
                                    const trt_resolver* rs);

// Return the guest's entry RVA (collected, not called — the host guest loop
// runs it). 0 when the image is null.
u64 trt_universal_guest_entry(trt_image* img);

// Free a universal guest image (with its backend).
void trt_universal_guest_unload(const trt_backend* be, trt_image* img);

#endif  // TIRAMISU_UNIVERSAL_GUEST_H
