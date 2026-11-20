// tiramisu/src/guest/delayimp32.c
// Minimal x86 delay-load helper for the M7 Phase 1 gate guest.
// lld-link /DELAYLOAD references __delayLoadHelper2 (stdcall, @8); the real
// helper lives in the MSVC CRT (delayimp.lib). This freestanding guest has
// no CRT, so it provides its own. NEVER CALLED in Phase 1 (load-only: the
// loader binds delay imports eagerly at map time, so the delay thunks are
// never hit). If it ever runs it returns NULL (load failed) — fail-visible.
typedef unsigned int u32;
void* __stdcall __delayLoadHelper2(void* desc, void* addr) {
  (void)desc; (void)addr;
  return 0;
}
