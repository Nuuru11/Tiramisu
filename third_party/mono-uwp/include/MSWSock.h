/* Shim: the xwin SDK splat lowercases this header's filename
 * (mswsock.h) while Mono's w32socket.c includes it as <MSWSock.h>.
 * Windows filesystems are case-insensitive so nobody notices there; on
 * the case-sensitive Linux build host the include fails. This shim
 * (found first via -I third_party/mono-uwp/include) forwards to the
 * real header. It changes no behaviour — it only fixes the filename
 * case. */
#pragma once
#include <mswsock.h>
