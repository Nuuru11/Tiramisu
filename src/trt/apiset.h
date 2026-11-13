// tiramisu/src/trt/apiset.h
// API-set redirection: modern Windows binaries import "api-ms-win-*.dll"
// contract names instead of real DLL names. On desktop the OS loader maps
// them through the apiset schema; in our loader the mapping is explicit.
//
// Why this matters: a 2015+ PC game almost never imports "kernel32.dll"
// directly for sync/interlocked/winrt helpers - it imports the contract
// name. If the contract name is unresolved the game does not start, so
// this table is load-path critical, not a nicety.
#ifndef TIRAMISU_APISET_H
#define TIRAMISU_APISET_H

#ifdef __cplusplus
extern "C" {
#endif

// Map an api-ms-* contract DLL name to the real DLL that implements it.
// Returns NULL if the contract is unknown (caller then tries the console's
// own resolver, which may know the schema).
const char* trt_apiset_target(const char* contract_name);

// True if the name looks like an api-ms contract DLL.
int trt_apiset_is_contract(const char* name);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_APISET_H
