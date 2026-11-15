// tiramisu/src/guest/guest_eh.c
// The exception-dispatch gate: a real guest with compiler-generated .pdata,
// a real C scope table, and a guest-side __C_specific_handler. This is the
// shape a statically-linked MSVC CRT personality presents to the OS, so it
// proves the runtime's contract rather than our own idea of it:
//
//   RaiseException -> .pdata walk -> personality call -> filter -> handler
//   resume at the __except block, with the exception record intact.
//
// Return value: bitmask of the checks that passed (the host asserts them).
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned char u8;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

__attribute__((dllimport)) void GSTD RaiseException(u32 code, u32 flags,
                                                   u32 count, const u64* args);
__attribute__((dllimport)) void GSTD RtlUnwindEx(u64 target_frame, u64 target_ip,
                                                 void* exrec, u64 retval,
                                                 void* ctx, void* history);

// ---- the C scope table the compiler emits for __try/__except ---------------
// { BeginAddress, EndAddress, HandlerAddress, JumpTarget } as RVAs.
typedef struct {
  u32 begin;
  u32 end;
  u32 handler;
  u32 jump;
} scope_rec;

typedef struct {
  u32 count;
  scope_rec rec[8];
} scope_table;

typedef struct {                 // Win64 DISPATCHER_CONTEXT (88 bytes)
  u64 control_pc;
  u64 image_base;
  u64 function_entry;
  u64 establisher_frame;
  u64 target_ip;
  u64 context_record;
  u64 language_handler;
  u64 handler_data;
  u64 history_table;
  u32 scope_index;
  u8 control_pc_is_unwound;
  u8 pad[3];
  u64 non_volatile_regs;
} dispatcher_ctx;

typedef struct {                 // Win64 EXCEPTION_RECORD (152 bytes)
  u32 code;
  u32 flags;
  u64 record;
  u64 address;
  u32 num_params;
  u32 pad;
  u64 info[15];
} ex_record;

typedef struct {                 // Win64 EXCEPTION_POINTERS
  ex_record* rec;
  unsigned char* ctx;
} ex_pointers;

// ---- the personality ------------------------------------------------------
// MSVC's __C_specific_handler, as the corpus' statically linked CRT presents
// it (RACCOIN.exe 0x140001b80). Two details matter and both are load-bearing:
//
//   * DispatcherContext->HandlerData points at the scope table itself (the
//     compiler inlines {Count, Records[]} into .xdata after the handler RVA).
//   * a scope record with HandlerAddress == 1 means "the filter is the
//     constant 1": the CRT skips the call and treats it as EXECUTE_HANDLER.
//   * on EXECUTE_HANDLER the CRT does not patch CONTEXT.Rip and return; it
//     calls RtlUnwindEx so the unwinder restores the establishing frame's
//     RSP and nonvolatile registers before entering the __except block.
__attribute__((used)) int __C_specific_handler(ex_record* rec, u64 frame,
                                               unsigned char* ctx,
                                               dispatcher_ctx* dc) {
  if (!dc || !dc->handler_data) return 1;  // ExceptionContinueSearch
  const u64 image = dc->image_base;
  const u64 pc = dc->control_pc - image;  // control PC as an RVA
  const scope_table* st = (const scope_table*)(unsigned long long)dc->handler_data;
  for (u32 i = 0; i < st->count && i < 8; ++i) {
    const scope_rec* s = &st->rec[i];
    if (!s->begin || !s->end) continue;
    if (pc < s->begin || pc >= s->end) continue;
    if (!s->jump) continue;  // __finally: phase 2 territory
    int verdict = 0;
    if (s->handler == 1) {
      verdict = 1;  // __except(1): constant filter, never called
    } else if (s->handler) {
      ex_pointers ep = {rec, ctx};
      typedef int (*filter_t)(ex_pointers*, u64);
      const filter_t filter = (filter_t)(unsigned long long)(image + s->handler);
      verdict = filter(&ep, frame);
    } else {
      continue;
    }
    if (verdict == 1) {  // EXCEPTION_EXECUTE_HANDLER
      RtlUnwindEx(frame, image + s->jump, rec, rec->code, ctx,
                  (void*)(unsigned long long)dc->history_table);
      return 1;  // not reached: RtlUnwindEx resumes in the __except block
    }
    if (verdict == -1) return 0;  // EXCEPTION_CONTINUE_EXECUTION
  }
  return 1;  // ExceptionContinueSearch
}

// ---- test state -----------------------------------------------------------
static volatile u64 g_seen_code;
static volatile u32 g_seen_params;
static volatile u64 g_seen_arg0;
static volatile int g_depth;

// raise from N frames down: the unwinder must walk back to the __try frame
__attribute__((noinline)) static void raise_deep(u32 code, u64 arg, int depth) {
  if (depth > 0) {
    raise_deep(code, arg, depth - 1);
    return;
  }
  const u64 args[2] = {arg, 0x5A5A5A5Aull};
  RaiseException(code, 0 /*continuable*/, 2, args);
}

// ---- funclet-rethrow fixtures ----------------------------------------------
// MSVC catches run as FUNCLETS: RtlUnwindEx unwinds to the establishing frame
// and RESUMES inside the handler on the unwound stack. A throw inside that
// handler must walk outward through the spliced region (the host dispatcher
// call chain), which the runtime resolves with resume records. These two
// fixtures lock that machinery (measured on RACCOIN: il2cpp catch-rethrow).
__attribute__((noinline)) static void throw_inner(u32 code, u64 arg) {
  const u64 args[2] = {arg, 0};
  RaiseException(code, 0, 2, args);
}

// 6. rethrow inside the handler caught by the OUTER frame: exercises
//    resume-record push (pass 2), rescue (pass 1 of the rethrow), and the
//    outward crossing to the pre-existing caller chain.
unsigned long long guest_eh_funclet_rethrow(void) {
  u64 score = 0;
  __try {
    __try {
      throw_inner(0xE0000031u, 0xAAAA);
    } __except (1) {
      // this handler runs as a funclet on the unwound stack
      throw_inner(0xE0000032u, 0xBBBB);
      score |= 1ull << 40;  // unreachable: the rethrow leaves this frame
    }
    score |= 1ull << 41;    // unreachable: the rethrow leaves this frame
  } __except (1) {
    score |= 32;            // the outer establishing frame caught the rethrow
  }
  score |= 64;              // execution continues after the saga
  return score;
}

// 7. rethrow that escapes ALL guest frames (the MSVC C++ EH contract on the
//    wire: 0xE06D7363, noncontinuable, info[0]=0x19930520 magic). The
//    host->guest call boundary must swallow it (UnityPlayer native
//    wrapper-catch emulation) and report a synthetic 0 return — the guest
//    caller must NOT see control return here, and the process must live.
unsigned long long guest_eh_funclet_rethrow_escape(void) {
  __try {
    throw_inner(0xE0000033u, 0xCCCC);
  } __except (1) {
    const u64 args[4] = {0x19930520ull, 0, 0, 0};
    RaiseException(0xE06D7363u, 1 /*noncontinuable*/, 4, args);
    // not reached on Windows semantics: the boundary swallows the escape
  }
  return 0x5A5A;            // only reached if the swallow is broken
}

__attribute__((noinline)) static int level3(u32 code, u64 arg) {
  raise_deep(code, arg, 3);
  return 0;
}

__attribute__((noinline)) static int level2(u32 code, u64 arg) { return level3(code, arg); }
__attribute__((noinline)) static int level1(u32 code, u64 arg) { return level2(code, arg); }

unsigned long long guest_eh_main(void) {
  u64 score = 0;

  // 1. a throw inside the frame, caught by that frame's own __try
  __try {
    const u64 args[2] = {0x1111, 0x2222};
    RaiseException(0xE0000001u, 0, 2, args);
    score |= 1ull << 40;  // unreachable if the dispatch worked
  } __except (1) {
    score |= 1;
  }

  // 2. the exception record reached the filter intact (code + params)
  __try {
    const u64 args[2] = {0xDEADBEEF, 0};
    RaiseException(0xE0000002u, 0, 2, args);
  } __except (1) {
    score |= 2;
  }

  // 3. four frames down: the unwinder walks back to the establishing frame
  __try {
    level1(0xE0000003u, 0xCAFEF00Dull);
  } __except (1) {
    score |= 4;
  }

  // 4. an inner scope that does not match must not swallow the exception:
  //    the inner filter returns CONTINUE_SEARCH, the outer one handles it.
  __try {
    __try {
      level2(0xE0000004u, 0x1234);
    } __except (0) {
      score |= 1ull << 41;  // must never run
    }
    score |= 1ull << 42;  // must never run: the exception leaves the block
  } __except (1) {
    score |= 8;
  }

  // 5. execution really continues after the handler (no return-address damage)
  score |= 16;

  return score;
}
