// tiramisu/src/trt/bridge_mmedia.h
// Small-gap multimedia/print bridges (M6 coverage batch).
//
// msacm32.dll  — the audio codec manager. Games with ADPCM/MP3-in-WAV
//                assets open conversion streams through acmStream*.
//                We report the format tags the console sink can absorb
//                (PCM only) and fail ACMERR_NOTPOSSIBLE visibly for
//                codecs we cannot decode — the engine then falls back
//                to PCM assets, its documented behavior.
// msvfw32.dll  — AVI video playback (intro logos). DrawDumb* functions
//                draw into an HDC the console does not have; the OEM
//                probes answer "no video devices" so engines skip AVI
//                logos instead of hanging.
// winspool.drv — printer probes. Some engines enumerate printers at
//                boot for "print screen" features; answering "no
//                printers" (ERROR_INSUFFICIENT... no: RC 0 + ERROR
//                set) is the documented no-printer shape.
#ifndef TIRAMISU_BRIDGE_MMEDIA_H
#define TIRAMISU_BRIDGE_MMEDIA_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_mmedia(void);

#ifdef __cplusplus
}
#endif

#endif
