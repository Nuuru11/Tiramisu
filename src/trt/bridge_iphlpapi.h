// tiramisu/src/trt/bridge_iphlpapi.h
// iphlpapi for the guest (bridge_iphlpapi.cpp).
//
// Adapter/interface tables are not trivially available in-process on the
// console, so GetAdaptersInfo/GetAdaptersAddresses/GetIfTable return honest
// no-data (ERROR_NO_DATA / zero adapters) with a named log. The ICMP probe
// surface (IcmpCreateFile/SendEcho/CloseHandle) UnityPlayer imports is a
// real handle record + honest failure at SendEcho (no raw sockets for the
// guest). Never silent-success with fake data.
#ifndef TIRAMISU_BRIDGE_IPHLPAPI_H
#define TIRAMISU_BRIDGE_IPHLPAPI_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register iphlpapi.dll as BUILTIN.
int trt_bridge_register_iphlpapi(void);

#ifdef __cplusplus
}
#endif

#endif
