# assemble/abi_switch_win64.s —— Win64 x64 换栈器（按 meta_abi_range 的 win_x64 固定帧）
#
# 调用约定（Microsoft x64 ABI）：
#   rcx = from_frame, rdx = to_frame
#   volatile    : rax rcx rdx r8 r9 r10 r11, xmm0..xmm5
#   nonvolatile : rbx rbp rdi rsi rsp r12..r15, xmm6..xmm15
#   MXCSR 的 [6:15]（控制位）与 x87 control word 也按非易失处理
#   栈在 prolog/epilog 之外必须 16 字节对齐；函数入口 rsp % 16 == 8
#
# 所以切换器要存：FP 块（xmm6..xmm15 + mxcsr + x87 CW）+ 8 个非易失 GPR + sp。
# 偏移相对 frame()，与 meta_abi_range.md 第 3 节一致：
#   +0   resume_pc      首次切入时 ret 吃掉的字（入口地址）
#   +8   exit_pc        入口函数正常返回时落到的假返回地址
#   +16  shadow[32]     Win64 home area
#   +48  xmm6..xmm15    10×16；+208 mxcsr，+212 x87 CW
#   +224 rbx rbp rdi rsi r12 r13 r14 r15（各 8 字节）
#   +288 sp             保存的 rsp，指向"恢复后要 ret 的那个返回地址"
#
# 为什么没有 .seh_proc / .pdata：这个函数把 rsp 换成另一条栈、ret 进另一个上下文，
# 换出之后它的"帧"已经不在当前栈上了。给它写 unwind 信息只会描述一个不存在的帧；
# 契约与 meta_abi_range.md 一致：异常不穿过换栈边界。
#
# 语法：GNU as，AT&T 默认（MinGW-W64 / COFF 目标）。COFF 的函数符号属性用
#   .def <name>; .scl 2; .type 32; .endef
# （scl 2 = external storage class，type 0x20 = 函数）。Linux 上那套
#   .type foo,@function / .size foo,.-foo 在 COFF 目标上不适用：as 会把 .size
#   报成 ".size pseudo-op used outside of .def/.endef" 并当作 junk。

	.text

# void abi_switch_swap(void* from_frame, void* to_frame);
	.globl	abi_switch_swap
	.def	abi_switch_swap;	.scl	2;	.type	32;	.endef
	.p2align 4
abi_switch_swap:
	# ---- 保存当前上下文到 from（rcx）----
	stmxcsr	208(%rcx)
	fnstcw	212(%rcx)
	movdqu	%xmm6, 48(%rcx)
	movdqu	%xmm7, 64(%rcx)
	movdqu	%xmm8, 80(%rcx)
	movdqu	%xmm9, 96(%rcx)
	movdqu	%xmm10, 112(%rcx)
	movdqu	%xmm11, 128(%rcx)
	movdqu	%xmm12, 144(%rcx)
	movdqu	%xmm13, 160(%rcx)
	movdqu	%xmm14, 176(%rcx)
	movdqu	%xmm15, 192(%rcx)
	movq	%rbx, 224(%rcx)
	movq	%rbp, 232(%rcx)
	movq	%rdi, 240(%rcx)
	movq	%rsi, 248(%rcx)
	movq	%r12, 256(%rcx)
	movq	%r13, 264(%rcx)
	movq	%r14, 272(%rcx)
	movq	%r15, 280(%rcx)
	# 不 push：此刻 [rsp] 就是本函数被 call 时压的返回地址，恢复后直接 ret 回原位。
	movq	%rsp, 288(%rcx)

	# ---- 从 to（rdx）恢复，最后换栈 + ret ----
	ldmxcsr	208(%rdx)
	fldcw	212(%rdx)
	movdqu	48(%rdx), %xmm6
	movdqu	64(%rdx), %xmm7
	movdqu	80(%rdx), %xmm8
	movdqu	96(%rdx), %xmm9
	movdqu	112(%rdx), %xmm10
	movdqu	128(%rdx), %xmm11
	movdqu	144(%rdx), %xmm12
	movdqu	160(%rdx), %xmm13
	movdqu	176(%rdx), %xmm14
	movdqu	192(%rdx), %xmm15
	movq	224(%rdx), %rbx
	movq	232(%rdx), %rbp
	movq	240(%rdx), %rdi
	movq	248(%rdx), %rsi
	movq	256(%rdx), %r12
	movq	264(%rdx), %r13
	movq	272(%rdx), %r14
	movq	280(%rdx), %r15
	movq	288(%rdx), %rsp
	ret

# void abi_switch_exit(void) —— 入口函数正常返回后的落点。
# 此时 rsp % 16 == 0（ret 弹掉 exit_pc 之后的状态），不是普通被 call 的入口，
# 所以先减出 32 字节 outgoing shadow space，再 call 一个不返回的完成处理函数。
# 这个 trampoline 自己不能再对未定义的下一槽 ret。
	.globl	abi_switch_exit
	.def	abi_switch_exit;	.scl	2;	.type	32;	.endef
	.p2align 4
abi_switch_exit:
	movq	abi_switch_finish_slot(%rip), %rax
	subq	$32, %rsp
	call	*%rax
	ud2
