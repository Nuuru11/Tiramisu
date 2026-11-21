/* win64.S -- GNU as translation of Mono's mono/utils/win64.asm
 *
 * Upstream: mono/mono, tag mono-6.8.0.105, mono/utils/win64.asm (MIT).
 * That file is MASM and is NOT in the release tarball (it lives in git
 * only); clang-cl cannot assemble MASM, so this is a mechanical
 * translation to GNU as syntax (Intel syntax, noprefix) with an
 * identical instruction stream and identical struct offsets.
 *
 * Defines:
 *   mono_context_get_current -- saves all GPRs + RSP + RIP into a
 *       MonoContext (mono/utils/mono-context.h). MSVC has no inline
 *       asm on x64, so Mono keeps it in a separate .asm file.
 *   copy_stack_data_internal_win32_wrapper -- saves the nonvolatile
 *       XMM6-15 + GPRs into a MonoBuiltinUnwindInfo, then tail-calls
 *       the CopyStackDataFunc (r9). Core cooperative-GC-suspend
 *       machinery; cannot be written in C (naked register capture).
 *
 * Static interp transition stubs (Tiramisu UWP, patches 31/32):
 *   mono_interp_to_native_trampoline_static -- precompiled equivalent
 *       of mono_arch_get_interp_to_native_trampoline (tramp-amd64.c):
 *       takes (callme, CallContext*) and performs the Win64 native call.
 *   mono_native_to_interp_trampoline_static -- precompiled equivalent
 *       of mono_arch_get_native_to_interp_trampoline: saves the native
 *       args into a CallContext and calls ftndesc->addr(ctx, ftndesc->arg).
 *   mono_ftnptr_thunks + mono_ftnptr_table -- fixed pool of 256 entry
 *       thunks; each loads its MonoFtnDesc* into r10 and jumps to the
 *       native->interp stub. The JIT equivalent (mono_arch_get_static_
 *       rgctx_trampoline) bakes the descriptor pointer in as an
 *       immediate, which static code cannot do, so the pointer lives
 *       in the table (filled at runtime by mono_uwp_alloc_ftnptr_thunk
 *       in interp.c).
 *
 * The AppContainer forbids executing dynamically generated code, so the
 * interpreter uses these image-backed stubs instead of the JIT
 * trampolines on UWP (see patches 31/32). Both stubs are mechanical
 * translations of the tramp-amd64.c emitters, so the C side (which
 * builds and reads the CallContext) is unchanged. CallContext layout
 * (mono/mini/mini-amd64.h): gregs[17] @ 0, fregs[16] @ 136,
 * stack_size @ 264, stack @ 272, sizeof 280. gregs[] is indexed by the
 * AMD64_Reg_No enum (mono/arch/amd64/amd64-codegen.h): RAX=0, RCX=1,
 * RDX=2, R8=8, R9=9, R10=10 (MONO_ARCH_RGCTX_REG). MonoFtnDesc
 * (mono/mini/mini.h): addr @ 0, arg @ 8.
 *
 * The first two functions are leaf functions (no stack use, no calls
 * except the tail jump), so no .pdata/.xdata unwind info is emitted --
 * the unwinder treats them as leaves, which is correct: the wrapper
 * has no frame, so the return address on the stack is the caller's.
 * The two transition stubs below are NOT leaves (they call out), so
 * they carry .seh unwind info (.pdata/.xdata) describing their
 * prologues. The ftnptr thunks are pure jumps (no frame, no calls)
 * and need none: the native caller's return address stays on top of
 * the stack across the jump, so the unwinder never sees a thunk frame.
 *
 * Assembled with:
 *   clang-cl -target x86_64-pc-windows-msvc -c win64.S -o win64.obj
 */
	.intel_syntax noprefix
	.text

	.globl mono_context_get_current
mono_context_get_current:
	/* rcx has the ctx ptr */
	mov [rcx + 0x00], rax
	mov [rcx + 0x08], rcx
	mov [rcx + 0x10], rdx
	mov [rcx + 0x18], rbx
	mov [rcx + 0x28], rbp
	mov [rcx + 0x30], rsi
	mov [rcx + 0x38], rdi
	mov [rcx + 0x40], r8
	mov [rcx + 0x48], r9
	mov [rcx + 0x50], r10
	mov [rcx + 0x58], r11
	mov [rcx + 0x60], r12
	mov [rcx + 0x68], r13
	mov [rcx + 0x70], r14
	mov [rcx + 0x78], r15
	lea rax, [rsp+8]
	mov [rcx + 0x20], rax
	mov rax, qword ptr [rsp]
	mov [rcx + 0x80], rax
	ret

	/* Implementation of __builtin_unwind_init under MSVC, dumping
	 * nonvolatile registers into MonoBuiltinUnwindInfo *. */
	.globl copy_stack_data_internal_win32_wrapper
copy_stack_data_internal_win32_wrapper:
	/* rcx MonoThreadInfo *
	 * rdx MonoStackData *
	 * r8 MonoBuiltinUnwindInfo *
	 * r9 CopyStackDataFunc */
	movaps xmmword ptr [r8 + 0x00], xmm6
	movaps xmmword ptr [r8 + 0x10], xmm7
	movaps xmmword ptr [r8 + 0x20], xmm8
	movaps xmmword ptr [r8 + 0x30], xmm9
	movaps xmmword ptr [r8 + 0x40], xmm10
	movaps xmmword ptr [r8 + 0x50], xmm11
	movaps xmmword ptr [r8 + 0x60], xmm12
	movaps xmmword ptr [r8 + 0x70], xmm13
	movaps xmmword ptr [r8 + 0x80], xmm14
	movaps xmmword ptr [r8 + 0x90], xmm15
	mov qword ptr [r8 + 0xA0], rbx
	mov qword ptr [r8 + 0xA8], rsi
	mov qword ptr [r8 + 0xB0], rdi
	mov qword ptr [r8 + 0xB8], r12
	mov qword ptr [r8 + 0xC0], r13
	mov qword ptr [r8 + 0xC8], r14
	mov qword ptr [r8 + 0xD0], r15
	mov qword ptr [r8 + 0xD8], rbp
	/* tailcall, all parameters passed through to CopyStackDataFunc. */
	jmp r9

	/* Static interp->native transition (patch 31): mechanical
	 * translation of mono_arch_get_interp_to_native_trampoline
	 * (mono/mini/tramp-amd64.c, TARGET_WIN32 branch). The signature
	 * walk is done in C by mono_arch_set_native_call_context_args /
	 * mono_arch_get_native_call_context_ret (mini-amd64.c); this stub
	 * only shuffles the CallContext into the Win64 calling convention.
	 *
	 *   rcx = target address (callme), rdx = CallContext* (margs).
	 * On return, gregs[RAX] / fregs[0] in the CallContext hold the
	 * return value. The only caller-saved registers used as temps
	 * are rcx/rdx/r8/rax -- the same temps the JIT emitter uses --
	 * so no callee-saved register is touched. The dynamic
	 * `sub rsp, rax` is invisible to the unwinder: the frame pointer
	 * (rbp) restores rsp, exactly as MSVC emits for alloca. */
	.globl mono_interp_to_native_trampoline_static
	.seh_proc mono_interp_to_native_trampoline_static
mono_interp_to_native_trampoline_static:
	push rbp
	.seh_pushreg rbp
	mov rbp, rsp
	.seh_setframe rbp, 0
	sub rsp, 16
	.seh_stackalloc 16
	.seh_endprologue
	/* save CallContext* and target address on the stack */
	mov [rbp - 8], rdx
	mov [rbp - 16], rcx
	/* r11 = CallContext* */
	mov r11, rdx
	/* allocate the stack space necessary for the call */
	mov rax, [r11 + 264]
	sub rsp, rax
	/* copy stack args from the CallContext: rcx = dest, rdx = source */
	mov rcx, rsp
	mov rdx, [r11 + 272]
.Linterp_to_native_copy:
	test rax, rax
	jz .Linterp_to_native_copy_done
	mov r8, [rdx]
	mov [rcx], r8
	add rcx, 8
	add rdx, 8
	sub rax, 8
	jmp .Linterp_to_native_copy
.Linterp_to_native_copy_done:
	/* set all general purpose registers from CallContext */
	mov rcx, [r11 + 8]
	mov rdx, [r11 + 16]
	mov r8, [r11 + 64]
	mov r9, [r11 + 72]
	/* set all floating registers from CallContext */
	movsd xmm0, qword ptr [r11 + 136]
	movsd xmm1, qword ptr [r11 + 144]
	movsd xmm2, qword ptr [r11 + 152]
	movsd xmm3, qword ptr [r11 + 160]
	/* call into native function */
	mov r11, [rbp - 16]
	call r11
	/* save all return registers in the CallContext */
	mov r11, [rbp - 8]
	mov [r11 + 0], rax
	movsd qword ptr [r11 + 136], xmm0
	lea rsp, [rbp]
	pop rbp
	ret
	.seh_endproc

	/* Static native->interp transition (patch 32): mechanical
	 * translation of mono_arch_get_native_to_interp_trampoline
	 * (mono/mini/tramp-amd64.c, TARGET_WIN32 branch). Called by
	 * native code with the delegate signature; r10 holds the
	 * MonoFtnDesc* (set by the ftnptr thunk). Saves the native args
	 * into a CallContext on the stack, calls
	 * ftndesc->addr(&ctx, ftndesc->arg) -- interp_entry_from_trampoline
	 * (interp.c), which runs the interpreted method and writes the
	 * return value back into the CallContext -- then restores the
	 * return registers. Frame: 32 bytes of home space for the callee
	 * + sizeof(CallContext)=280, aligned to 16 = 320; the CallContext
	 * sits at rsp+32. */
	.globl mono_native_to_interp_trampoline_static
	.seh_proc mono_native_to_interp_trampoline_static
mono_native_to_interp_trampoline_static:
	push rbp
	.seh_pushreg rbp
	mov rbp, rsp
	.seh_setframe rbp, 0
	sub rsp, 320
	.seh_stackalloc 320
	.seh_endprologue
	/* save all general purpose registers into the CallContext */
	mov [rsp + 32 + 8], rcx
	mov [rsp + 32 + 16], rdx
	mov [rsp + 32 + 64], r8
	mov [rsp + 32 + 72], r9
	/* save all floating registers into the CallContext */
	movsd qword ptr [rsp + 32 + 136], xmm0
	movsd qword ptr [rsp + 32 + 144], xmm1
	movsd qword ptr [rsp + 32 + 152], xmm2
	movsd qword ptr [rsp + 32 + 160], xmm3
	/* set the stack pointer to the value at call site (rbp + 16 skips
	 * the saved rbp and the return address, landing on the caller's
	 * home space -- the C reader skips the first 4 slots, same as the
	 * JIT trampoline). */
	lea r11, [rbp + 16]
	mov [rsp + 32 + 272], r11
	/* call interp_entry with the ccontext and rmethod as arguments */
	lea rcx, [rsp + 32]
	mov rdx, [r10 + 8]
	mov r11, [r10 + 0]
	call r11
	/* load the return values from the context */
	mov rax, [rsp + 32 + 0]
	movsd xmm0, qword ptr [rsp + 32 + 136]
	/* reset stack and return */
	lea rsp, [rbp]
	pop rbp
	ret
	.seh_endproc

	/* Fixed pool of native->interp entry thunks (patch 32). Each
	 * thunk loads its MonoFtnDesc* from mono_ftnptr_table[i] into
	 * r10 (MONO_ARCH_RGCTX_REG) and jumps to the static native->interp
	 * trampoline. The table is filled at runtime by
	 * mono_uwp_alloc_ftnptr_thunk (interp.c); the thunks are static,
	 * image-backed code. Thunk i is exactly 12 bytes (7-byte
	 * RIP-relative load + 5-byte relative jump); interp.c computes
	 * thunk addresses as mono_ftnptr_thunks + i * 12 (verified by
	 * disassembly after the build -- see mono-static-stubs.md).
	 * MONO_UWP_FTNPTR_COUNT in interp.c must match the .rept count
	 * here (256). */
	.globl mono_ftnptr_thunks
	.balign 16
mono_ftnptr_thunks:
	.set mono_ftnptr_slot, 0
	.rept 256
	mov r10, qword ptr [rip + mono_ftnptr_table + mono_ftnptr_slot * 8]
	jmp mono_native_to_interp_trampoline_static
	.set mono_ftnptr_slot, mono_ftnptr_slot + 1
	.endr
	/* NOTE: each thunk must be exactly 12 bytes (7-byte RIP-relative
	 * load + 5-byte relative jump); interp.c computes thunk addresses
	 * as mono_ftnptr_thunks + i * 12. clang's integrated assembler
	 * rejects `.if (. - symbol)` here (not an absolute expression),
	 * so the size is verified by disassembly after the build (see
	 * mono-static-stubs.md) rather than by an assemble-time assert. */

	.data
	.balign 8
	.globl mono_ftnptr_table
mono_ftnptr_table:
	.zero 256 * 8
