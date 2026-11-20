/* faudio_uwp_compat.h -- compile-time bridge for FAudio under UWP.
 *
 * Force-included (via /FI) by tools/build-host.sh for every TU of the UWP
 * host build. It declares exactly one function the SDK hides from the APP
 * partition but FAudio's core needs:
 *
 *   lstrcpynA -- declared only under WINAPI_PARTITION_DESKTOP | SYSTEM
 *   (winbase.h), but FACT.c uses it via FAudio_strlcpy
 *   (FAudio_internal.h). The definition lives in FAudio_platform_uwp.c
 *   (a real strncpy-with-NUL implementation, documented semantics).
 *
 * This header declares no other names and changes no behavior for TUs
 * that do not call lstrcpynA. It must NOT grow desktop-API shims: any
 * further hidden declaration is a sign the calling code needs a real UWP
 * path instead.
 */
#ifndef FAUDIO_UWP_COMPAT_H
#define FAUDIO_UWP_COMPAT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

LPSTR WINAPI lstrcpynA(LPSTR lpString1, LPCSTR lpString2, int iMaxLength);

#ifdef __cplusplus
}
#endif

#endif /* FAUDIO_UWP_COMPAT_H */
