// tiramisu/src/trt/pc2xb_translator.h
// Phase 3: PC->Xbox API translator.
//
// Translates a PC game's import surface (dumped by
// trt_pe_enumerate_imports) to its Xbox equivalent:
//  - Direct3D 9/8/legacy -> d3d11 (the Xbox universal GPU path);
//  - DSound / WinMM -> xaudio2 (deprecated off Xbox);
//  - unmapped modules -> "host:"-serviced by the Tiramisu universal host.
// Mapped imports may change module names (d3d9.dll -> d3d11.dll); symbol
// names are unchanged (the Win32/MSVC ABI is the same on Xbox Series X).
#ifndef TIRAMISU_PC2XB_TRANSLATOR_H
#define TIRAMISU_PC2XB_TRANSLATOR_H

#include "trt.h"
#include "pe.h"

// Translate a PC import profile into an Xbox import profile. Fills `xb`
// (bounded by 1024 entries) and returns the translated count, or -1 on
// usage errors.
int trt_pc2xb_translate_imports(const trt_universal_profile* pc,
                                trt_universal_profile* xb);

// Translate a PC game executable: enumerates the PC import surface,
// translates it, and writes <out_prefix>_imports.json, <out_prefix>_imports.csv
// and <out_prefix>_profile.json. Returns the translated import count, or -1.
int trt_pc2xb_translate_file(const char* pc_exe, const char* out_prefix);

#endif  // TIRAMISU_PC2XB_TRANSLATOR_H
