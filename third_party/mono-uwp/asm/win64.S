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
 * Both are leaf functions (no stack use, no calls except the tail
 * jump), so no .pdata/.xdata unwind info is emitted -- the unwinder
 * treats them as leaves, which is correct: the wrapper has no frame,
 * so the return address on the stack is the caller's.
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
