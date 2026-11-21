// tiramisu/src/trt/bridge_ktmw32.cpp
// Kernel Transaction Manager (ktmw32.dll) bridge.
//
// discord_game_sdk.dll imports CreateTransaction + CommitTransaction at LOAD
// time (measured: corpus dry-run, ktmw32 0/2 bound), so the missing exports
// were a hard load failure for every game shipping that SDK — not a runtime
// option.
//
// What is REAL: a transaction handle is a real kernel32 event handle, so
// CloseHandle/GetTransactionId behave the way the guest expects and the
// handle cannot collide with a Win32 pseudo-handle.
//
// What is NOT: isolation. The VFS applies writes immediately, so
// CommitTransaction answers TRUE truthfully ("your writes are durable") and
// says so in the log. RollbackTransaction cannot undo anything, so it FAILS
// NAMED instead of pretending to roll back.
#include "bridge_ktmw32.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <string.h>

#include <mutex>

extern "C" {
u64 TRT_STDCALL trt_CreateEventW(void* attrs, bool manual, bool initial,
                                 const u16* name);
void TRT_STDCALL trt_SetLastError(u32 e);
}

namespace {

constexpr u32 KTM_MAX_TRANSACTIONS = 32;
constexpr u32 KTM_ERROR_INVALID_HANDLE = 6;
constexpr u32 KTM_ERROR_NOT_ENOUGH_MEMORY = 8;
constexpr u32 KTM_ERROR_CALL_NOT_IMPLEMENTED = 120;

struct tx_rec {
  u64 handle;    // real kernel32 event handle (CloseHandle works on it)
  bool used;
  bool committed;
  u64 id_lo;     // stable pseudo-UOW so GetTransactionId has an answer
  u64 id_hi;
};

tx_rec g_tx[KTM_MAX_TRANSACTIONS];
std::mutex g_tx_mu;

void line(const char* s) { trt_core_log(s); }

// caller holds g_tx_mu
int find_tx_locked(u64 h) {
  if (!h) return -1;
  for (u32 i = 0; i < KTM_MAX_TRANSACTIONS; ++i)
    if (g_tx[i].used && g_tx[i].handle == h) return (int)i;
  return -1;
}

void* invalid_handle() { return (void*)(uintptr_t)~0ull; }

}  // namespace

extern "C" {

void* TRT_STDCALL trt_CreateTransaction(void* attrs, const void* uow,
                                        u32 create_options, u32 isolation_level,
                                        u32 isolation_flags, u32 timeout,
                                        const u16* description) {
  (void)attrs; (void)uow; (void)create_options; (void)isolation_level;
  (void)isolation_flags; (void)timeout; (void)description;
  const u64 h = trt_CreateEventW(nullptr, true, false, nullptr);
  if (!h || h == (u64)~0ull) {
    line("ktmw32: CreateTransaction -> INVALID_HANDLE_VALUE "
         "(kernel event alloc failed)");
    trt_SetLastError(KTM_ERROR_NOT_ENOUGH_MEMORY);
    return invalid_handle();
  }
  std::lock_guard<std::mutex> lk(g_tx_mu);
  int slot = -1;
  for (u32 i = 0; i < KTM_MAX_TRANSACTIONS; ++i) {
    if (!g_tx[i].used) {
      slot = (int)i;
      break;
    }
  }
  if (slot < 0) {
    line("ktmw32: CreateTransaction -> INVALID_HANDLE_VALUE "
         "(transaction table full, 32 live)");
    trt_SetLastError(KTM_ERROR_NOT_ENOUGH_MEMORY);
    return invalid_handle();
  }
  g_tx[slot].used = true;
  g_tx[slot].handle = h;
  g_tx[slot].committed = false;
  g_tx[slot].id_lo = h ^ 0x5458525458494d41ull;  // "AMIXTRXT"
  g_tx[slot].id_hi = 0x5553494d41524954ull;      // "TIRAMISU"
  char buf[160];
  snprintf(buf, sizeof(buf),
           "ktmw32: CreateTransaction -> 0x%llx (real handle; VFS writes are "
           "NOT isolated - commit-only)",
           (unsigned long long)h);
  line(buf);
  return (void*)(uintptr_t)h;
}

int TRT_STDCALL trt_CommitTransaction(void* handle) {
  const u64 h = (u64)(uintptr_t)handle;
  std::lock_guard<std::mutex> lk(g_tx_mu);
  const int i = find_tx_locked(h);
  if (i < 0) {
    line("ktmw32: CommitTransaction -> FALSE (invalid transaction handle)");
    trt_SetLastError(KTM_ERROR_INVALID_HANDLE);
    return 0;
  }
  g_tx[i].committed = true;
  char buf[160];
  snprintf(buf, sizeof(buf),
           "ktmw32: CommitTransaction 0x%llx -> TRUE "
           "(writes were already applied; no isolation to commit)",
           (unsigned long long)h);
  line(buf);
  return 1;
}

int TRT_STDCALL trt_RollbackTransaction(void* handle) {
  const u64 h = (u64)(uintptr_t)handle;
  std::lock_guard<std::mutex> lk(g_tx_mu);
  const int i = find_tx_locked(h);
  if (i < 0) {
    line("ktmw32: RollbackTransaction -> FALSE (invalid transaction handle)");
    trt_SetLastError(KTM_ERROR_INVALID_HANDLE);
    return 0;
  }
  char buf[176];
  snprintf(buf, sizeof(buf),
           "ktmw32: RollbackTransaction 0x%llx -> FALSE "
           "(VFS writes are applied immediately: cannot undo)",
           (unsigned long long)h);
  line(buf);
  trt_SetLastError(KTM_ERROR_CALL_NOT_IMPLEMENTED);
  return 0;
}

int TRT_STDCALL trt_GetTransactionId(void* handle, void* guid_out) {
  const u64 h = (u64)(uintptr_t)handle;
  std::lock_guard<std::mutex> lk(g_tx_mu);
  const int i = find_tx_locked(h);
  if (i < 0) {
    line("ktmw32: GetTransactionId -> FALSE (invalid transaction handle)");
    trt_SetLastError(KTM_ERROR_INVALID_HANDLE);
    return 0;
  }
  if (guid_out) {
    // GUID layout: Data1(4) Data2(2) Data3(2) Data4(8), little-endian words.
    u8* g = (u8*)guid_out;
    const u64 lo = g_tx[i].id_lo;
    const u64 hi = g_tx[i].id_hi;
    for (int b = 0; b < 4; ++b) g[b] = (u8)(lo >> (8 * b));
    for (int b = 0; b < 2; ++b) g[4 + b] = (u8)(lo >> (32 + 8 * b));
    for (int b = 0; b < 2; ++b) g[6 + b] = (u8)(hi >> (8 * b));
    for (int b = 0; b < 8; ++b) g[8 + b] = (u8)(hi >> (16 + 8 * b));
  }
  return 1;
}

int trt_bridge_register_ktmw32(void) {
  static const trt_named_fn k_fns[] = {
      {"CreateTransaction", (void*)&trt_CreateTransaction},
      {"CommitTransaction", (void*)&trt_CommitTransaction},
      {"RollbackTransaction", (void*)&trt_RollbackTransaction},
      {"GetTransactionId", (void*)&trt_GetTransactionId},
  };
  trt_module_register("ktmw32.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
