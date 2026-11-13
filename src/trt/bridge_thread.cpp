// tiramisu/src/trt/bridge_thread.cpp
// Threads and synchronisation for the guest.
//
// Unity, Unreal and most engines are heavily multi-threaded: they create
// worker threads, wait on events, and use critical sections. Without these
// a game either hangs on the first WaitForSingleObject or races.
//
// On the console the guest's thread is a REAL thread: its start routine is
// Win64 code, so the OS calls it with the Win64 ABI and it runs natively.
// In the Linux test harness the host ABI is SysV, so the thread is started
// through trt_call_guest (which marshals the Win64 ABI).
#include "trt.h"
#include "thunk.h"
#include "images.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <pthread.h>
#endif

namespace {

constexpr int MAX_OBJECTS = 128;

enum obj_kind { OBJ_NONE = 0, OBJ_EVENT, OBJ_MUTEX, OBJ_SEMAPHORE, OBJ_THREAD };

struct sync_object {
  obj_kind kind;
  bool signalled;
  bool manual_reset;
  int lock_count;
  bool alive;
#if defined(_WIN32)
  HANDLE win_handle;
#else
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool cond_init;
#endif
};

sync_object g_objects[MAX_OBJECTS];

sync_object* object_for(u64 h) {
  const u32 v = (u32)h;
  if ((v & 0xFFF00000u) != 0x5B000000u) return nullptr;
  const u32 idx = v & 0xFFFFu;
  if (idx >= MAX_OBJECTS || g_objects[idx].kind == OBJ_NONE) return nullptr;
  return &g_objects[idx];
}

u64 alloc_object(obj_kind kind) {
  for (int i = 0; i < MAX_OBJECTS; ++i) {
    if (g_objects[i].kind != OBJ_NONE) continue;
    memset(&g_objects[i], 0, sizeof(sync_object));
    g_objects[i].kind = kind;
#if !defined(_WIN32)
    pthread_mutex_init(&g_objects[i].mutex, nullptr);
    pthread_cond_init(&g_objects[i].cond, nullptr);
    g_objects[i].cond_init = true;
#endif
    return 0x5B000000u | (u32)i;
  }
  return 0;
}

void destroy_object(sync_object* o) {
  if (!o) return;
#if !defined(_WIN32)
  if (o->cond_init) {
    pthread_cond_destroy(&o->cond);
    pthread_mutex_destroy(&o->mutex);
  }
#endif
  memset(o, 0, sizeof(*o));
}

#if !defined(_WIN32)
// the guest's thread routine is Win64 code: call it with the Win64 ABI
struct thread_start {
  void* routine;
  u64 arg;
  u64 object;   // the sync object the guest is waiting on
};
void* thread_trampoline(void* raw) {
  thread_start* ts = (thread_start*)raw;
  const void* routine = ts->routine;
  const u64 arg = ts->arg;
  const u64 object = ts->object;
  free(ts);
  const u64 ret = trt_call_guest((void*)routine, arg, 0, 0, 0);
  // the thread finished: release anyone waiting on it
  sync_object* o = object_for(object);
  if (o) {
    o->alive = false;
    o->signalled = true;
  }
  return (void*)(uintptr_t)ret;
}
#endif

} // namespace

extern "C" {

// ABI rule (see pe.h): every declaration of a TRT_STDCALL function must
// itself carry TRT_STDCALL — these are defined ms_abi in bridge_kernel32.cpp.
u64 TRT_STDCALL trt_GetTickCount64(void);
void TRT_STDCALL trt_Sleep(u32 ms);

// CreateThread(attrs, stack, start, param, flags, tid)
u64 TRT_STDCALL trt_CreateThread(void* attrs, void* stack_size, void* routine,
                                    void* param, u32 flags, u32* tid) {
  (void)attrs; (void)flags;
  if (!routine) return 0;
  const u64 obj = alloc_object(OBJ_THREAD);
  if (!obj) return 0;
  sync_object* o = object_for(obj);
  o->alive = true;

#if defined(_WIN32)
  HANDLE t = CreateThread(nullptr, (SIZE_T)stack_size,
                          (LPTHREAD_START_ROUTINE)routine, param, 0, nullptr);
  if (!t) {
    destroy_object(o);
    return 0;
  }
  o->win_handle = t;
  if (tid) *tid = (u32)(uintptr_t)t;
#else
  thread_start* ts = (thread_start*)malloc(sizeof(thread_start));
  if (!ts) {
    destroy_object(o);
    return 0;
  }
  ts->routine = routine;
  ts->arg = (u64)(uintptr_t)param;
  ts->object = obj;
  pthread_t th;
  if (pthread_create(&th, nullptr, &thread_trampoline, ts) != 0) {
    free(ts);
    destroy_object(o);
    return 0;
  }
  pthread_detach(th);
  if (tid) *tid = (u32)(uintptr_t)th;
#endif
  return obj;
}

u32 TRT_STDCALL trt_ExitThread(u32 code) {
#if defined(_WIN32)
  ExitThread(code);
#endif
  return code;
}

int TRT_STDCALL trt_TerminateThread(u64 thread, u32 code) {
  sync_object* o = object_for(thread);
  if (!o) return 0;
  o->alive = false;
#if defined(_WIN32)
  // TerminateThread is a desktop API: inside a UWP app it is not callable,
  // so the thread is only detached (games rarely force-kill a worker)
  if (o->win_handle) {
    CloseHandle(o->win_handle);
    o->win_handle = nullptr;
  }
#endif
  return 1;
}

// WaitForSingleObject(handle, ms): 0 signalled, 1 timeout, 2 abandoned
u32 TRT_STDCALL trt_WaitForSingleObject(u64 handle, u32 ms) {
  sync_object* o = object_for(handle);
  if (!o) return 1;
#if defined(_WIN32)
  if (o->kind == OBJ_THREAD && o->win_handle) {
    // the console host has a real thread: wait on the real handle
    const DWORD r = ::WaitForSingleObject(o->win_handle, ms);
    if (r == WAIT_OBJECT_0) return 0;
    if (r == WAIT_ABANDONED) return 2;
    return 1;
  }
#endif
  const u32 start = (u32)trt_GetTickCount64();
  for (;;) {
    if (o->kind == OBJ_EVENT && o->signalled) {
      if (!o->manual_reset) o->signalled = false;
      return 0;
    }
    if (o->kind == OBJ_THREAD && o->signalled) return 0;
    if (o->kind == OBJ_MUTEX && o->lock_count > 0) {
      o->lock_count = 0;
      return 0;
    }
    if (o->kind == OBJ_SEMAPHORE && o->lock_count > 0) {
      --o->lock_count;
      return 0;
    }
    if (ms == 0) return 1;
    const u32 now = (u32)trt_GetTickCount64();
    if ((now - start) >= ms) return 1;
    trt_Sleep(1);
  }
}

u32 TRT_STDCALL trt_WaitForMultipleObjects(u32 count, const u64* handles,
                                             bool wait_all, u32 ms) {
  u32 start = (u32)trt_GetTickCount64();
  for (;;) {
    u32 signalled = 0;
    for (u32 i = 0; i < count; ++i) {
      sync_object* o = object_for(handles[i]);
      if (o && ((o->kind == OBJ_EVENT && o->signalled) ||
                (o->kind == OBJ_MUTEX && o->lock_count > 0) ||
                (o->kind == OBJ_SEMAPHORE && o->lock_count > 0)))
        ++signalled;
    }
    if (wait_all ? (signalled == count) : (signalled > 0))
      return 0;
    if (ms == 0) return 1;
    if (((u32)trt_GetTickCount64() - start) >= ms) return 1;
    trt_Sleep(1);
  }
}

int TRT_STDCALL trt_GetExitCodeThread(u64 thread, u32* code) {
  sync_object* o = object_for(thread);
  if (!o) return 0;
  if (code) *code = 0;
  return 1;
}

// --- events ---
u64 TRT_STDCALL trt_CreateEventW(void* attrs, bool manual, bool initial,
                                    const u16* name) {
  (void)attrs; (void)name;
  const u64 h = alloc_object(OBJ_EVENT);
  if (!h) return 0;
  sync_object* o = object_for(h);
  o->manual_reset = manual;
  o->signalled = initial;
  return h;
}

int TRT_STDCALL trt_SetEvent(u64 handle) {
  sync_object* o = object_for(handle);
  if (!o) return 0;
  o->signalled = true;
  return 1;
}

int TRT_STDCALL trt_ResetEvent(u64 handle) {
  sync_object* o = object_for(handle);
  if (!o) return 0;
  o->signalled = false;
  return 1;
}

int TRT_STDCALL trt_PulseEvent(u64 handle) {
  sync_object* o = object_for(handle);
  if (!o) return 0;
  o->signalled = false;
  return 1;
}

// --- mutex ---
u64 TRT_STDCALL trt_CreateMutexW(void* attrs, bool initial_owner,
                                   const u16* name) {
  (void)attrs; (void)name;
  const u64 h = alloc_object(OBJ_MUTEX);
  if (!h) return 0;
  sync_object* o = object_for(h);
  o->lock_count = initial_owner ? 1 : 0;
  return h;
}

int TRT_STDCALL trt_ReleaseMutex(u64 handle) {
  sync_object* o = object_for(handle);
  if (!o) return 0;
  o->lock_count = 0;
  return 1;
}

// --- semaphore ---
u64 TRT_STDCALL trt_CreateSemaphoreW(void* attrs, u32 initial, u32 maximum,
                                        const u16* name) {
  (void)attrs; (void)name;
  const u64 h = alloc_object(OBJ_SEMAPHORE);
  if (!h) return 0;
  sync_object* o = object_for(h);
  o->lock_count = (int)initial;
  return h;
}

int TRT_STDCALL trt_ReleaseSemaphore(u64 handle, u32 count,
                                       u32* previous) {
  sync_object* o = object_for(handle);
  if (!o) return 0;
  if (previous) *previous = (u32)o->lock_count;
  o->lock_count += (int)count;
  return 1;
}

// --- critical sections (per-thread, like Windows) ---
struct crit_section {
  bool locked;
  u32 owner;
};
static thread_local u32 g_tls_self = 0;

int TRT_STDCALL trt_InitializeCriticalSection(void* cs) {
  if (!cs) return 0;
  memset(cs, 0, sizeof(crit_section));
  return 1;
}

int TRT_STDCALL trt_EnterCriticalSection(void* cs) {
  if (!cs) return 0;
  crit_section* c = (crit_section*)cs;
  while (c->locked && c->owner != g_tls_self) trt_Sleep(1);
  c->locked = true;
  c->owner = g_tls_self;
  return 1;
}

int TRT_STDCALL trt_LeaveCriticalSection(void* cs) {
  if (!cs) return 0;
  crit_section* c = (crit_section*)cs;
  c->locked = false;
  return 1;
}

int TRT_STDCALL trt_DeleteCriticalSection(void* cs) {
  (void)cs;
  return 1;
}

// --- TLS ---
u32 TRT_STDCALL trt_TlsAlloc(void) {
  static u32 next = 1;
  return next++;
}

int TRT_STDCALL trt_TlsGetValue(u32 index) {
  (void)index;
  return 0;
}

int TRT_STDCALL trt_TlsSetValue(u32 index, void* value) {
  (void)index; (void)value;
  return 1;
}

int TRT_STDCALL trt_TlsFree(u32 index) {
  (void)index;
  return 1;
}

// close a synchronisation object (the kernel32 CloseHandle calls this first,
// then falls through to the file handles)
int TRT_STDCALL trt_sync_close(u64 handle) {
  sync_object* o = object_for(handle);
  if (!o) return 0;
  destroy_object(o);
  return 1;
}

int trt_bridge_register_threads(void) {
  static const trt_named_fn k_fns[] = {
      {"CreateThread", (void*)&trt_CreateThread},
      {"ExitThread", (void*)&trt_ExitThread},
      {"TerminateThread", (void*)&trt_TerminateThread},
      {"WaitForSingleObject", (void*)&trt_WaitForSingleObject},
      {"WaitForMultipleObjects", (void*)&trt_WaitForMultipleObjects},
      {"GetExitCodeThread", (void*)&trt_GetExitCodeThread},
      {"CreateEventW", (void*)&trt_CreateEventW},
      {"CreateEventA", (void*)&trt_CreateEventW},
      {"SetEvent", (void*)&trt_SetEvent},
      {"ResetEvent", (void*)&trt_ResetEvent},
      {"PulseEvent", (void*)&trt_PulseEvent},
      {"CreateMutexW", (void*)&trt_CreateMutexW},
      {"CreateMutexA", (void*)&trt_CreateMutexW},
      {"ReleaseMutex", (void*)&trt_ReleaseMutex},
      {"CreateSemaphoreW", (void*)&trt_CreateSemaphoreW},
      {"CreateSemaphoreA", (void*)&trt_CreateSemaphoreW},
      {"ReleaseSemaphore", (void*)&trt_ReleaseSemaphore},
      {"InitializeCriticalSection", (void*)&trt_InitializeCriticalSection},
      {"InitializeCriticalSectionAndSpinCount",
       (void*)&trt_InitializeCriticalSection},
      {"EnterCriticalSection", (void*)&trt_EnterCriticalSection},
      {"LeaveCriticalSection", (void*)&trt_LeaveCriticalSection},
      {"DeleteCriticalSection", (void*)&trt_DeleteCriticalSection},
      {"TlsAlloc", (void*)&trt_TlsAlloc},
      {"TlsGetValue", (void*)&trt_TlsGetValue},
      {"TlsSetValue", (void*)&trt_TlsSetValue},
      {"TlsFree", (void*)&trt_TlsFree},
  };
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_fns,
                       (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

} // extern "C"
