// tests/test_winmm_timer.cpp
// Gate: multimedia timers are REAL.
//
// timeSetEvent used to answer 0 ("no timer") because entering guest code from a
// host thread looked unsafe. It now runs a thread that sleeps to its deadline
// and fires. This test proves the observable contract without any guest image:
//
//   * timeSetEvent returns a nonzero id for a valid request, 0 for a null
//     callback (the one case the API itself rejects);
//   * a TIME_ONESHOT timer fires exactly once;
//   * a TIME_PERIODIC timer fires repeatedly;
//   * timeKillEvent joins the thread, stops the firing, and the timer leaves
//     the live set; an unknown id returns TIMERR_NOCANDO (97);
//   * the callback-type flags (EVENT_SET / EVENT_PULSE) are accepted.
//
// The event path is used because trt_SetEvent on a handle no event ever created
// simply returns false - the fire counter is what is being measured, and it is
// incremented for every delivery regardless of callback type.
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(__x86_64__) && !defined(_WIN32)
#define GUEST_ABI __attribute__((ms_abi))
#else
#define GUEST_ABI
#endif

extern "C" int trt_bridge_register_winmm(void);
extern "C" void* trt_resolve_module(void* image, const char* module,
                                    const char* symbol, unsigned ordinal);
extern "C" unsigned trt_timer_live(void);
extern "C" unsigned long long trt_timer_fires(void);

typedef unsigned(GUEST_ABI* set_event_fn)(unsigned delay_ms, unsigned res_ms,
                                          void* cb, unsigned long long user,
                                          unsigned flags);
typedef unsigned(GUEST_ABI* kill_event_fn)(unsigned id);

static int g_fail = 0;
static int g_pass = 0;

static void check(bool ok, const char* what) {
  if (ok) {
    ++g_pass;
    printf("  PASS %s\n", what);
  } else {
    ++g_fail;
    printf("  FAIL %s\n", what);
  }
}

// Poll the fire counter instead of sleeping a fixed time: a slow machine that
// takes 40ms to get a thread scheduled must not look like a broken timer.
static bool wait_fires(unsigned long long want, int timeout_ms) {
  for (int waited = 0; waited < timeout_ms; waited += 5) {
    if (trt_timer_fires() >= want) return true;
    usleep(5000);
  }
  return trt_timer_fires() >= want;
}

int main(void) {
  printf("test_winmm_timer: multimedia timers (timeSetEvent/timeKillEvent)\n");

  trt_bridge_register_winmm();
  auto set_ev = (set_event_fn)trt_resolve_module(nullptr, "winmm.dll",
                                                 "timeSetEvent", 0);
  auto kill_ev = (kill_event_fn)trt_resolve_module(nullptr, "winmm.dll",
                                                   "timeKillEvent", 0);
  check(set_ev != nullptr, "winmm!timeSetEvent resolves");
  check(kill_ev != nullptr, "winmm!timeKillEvent resolves");
  if (!set_ev || !kill_ev) {
    printf("RESULT: %d passed, %d failed\n", g_pass, g_fail);
    return 1;
  }

  // A null callback is rejected by the API itself.
  check(set_ev(10, 1, nullptr, 0, 0) == 0,
        "timeSetEvent(null callback) -> 0");

  // --- TIME_ONESHOT ---
  const unsigned long long before = trt_timer_fires();
  const unsigned oneshot = set_ev(10, 1, (void*)0x1234u, 0, 0x0010u /*EVENT_SET*/);
  check(oneshot != 0, "timeSetEvent(one-shot) -> nonzero id");
  check(trt_timer_live() >= 1, "one-shot timer is live after creation");
  check(wait_fires(before + 1, 2000), "one-shot timer fired");
  check(trt_timer_fires() == before + 1, "one-shot fired exactly once");
  check(kill_ev(oneshot) == 0, "timeKillEvent(one-shot) -> TIMERR_NOERROR");
  check(trt_timer_live() == 0, "one-shot timer left the live set");

  // A one-shot that already retired is no longer known.
  check(kill_ev(oneshot) == 97, "timeKillEvent(retired id) -> TIMERR_NOCANDO");

  // --- TIME_PERIODIC ---
  const unsigned long long p_before = trt_timer_fires();
  const unsigned periodic =
      set_ev(5, 1, (void*)0x1234u, 0, 0x0011u /*PERIODIC|EVENT_SET*/);
  check(periodic != 0, "timeSetEvent(periodic) -> nonzero id");
  check(wait_fires(p_before + 3, 2000), "periodic timer fired 3+ times");
  check(kill_ev(periodic) == 0, "timeKillEvent(periodic) -> TIMERR_NOERROR");
  const unsigned long long at_kill = trt_timer_fires();
  usleep(60000);
  check(trt_timer_fires() == at_kill, "no fires after timeKillEvent returns");
  check(trt_timer_live() == 0, "periodic timer left the live set");

  // --- TIME_PULSE is accepted too ---
  const unsigned long long u_before = trt_timer_fires();
  const unsigned pulse = set_ev(5, 1, (void*)0x1234u, 0, 0x0020u);
  check(pulse != 0, "timeSetEvent(EVENT_PULSE) accepted");
  check(wait_fires(u_before + 1, 2000), "EVENT_PULSE timer fired");
  check(kill_ev(pulse) == 0, "timeKillEvent(pulse) -> TIMERR_NOERROR");

  // --- unknown id ---
  check(kill_ev(999999u) == 97, "timeKillEvent(unknown id) -> TIMERR_NOCANDO");

  printf("RESULT: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
