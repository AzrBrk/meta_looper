# assemble/abi_switch_probe_win64.s —— 只给 examples/abi_switch_smoke.cpp 用的测试汇编
#
# 为什么要写成汇编：只测 C++ 局部变量不可靠。 -O2 会把常量比较折叠掉，编译器也不一定
# 真的用 xmm6..xmm15。这里把 18 个（10 个 xmm + 8 个 GPR）非易失寄存器填成魔数，
# 换一次栈再核对；整段过程没有编译器生成的指令。
#
# 魔数：xmm6..xmm15 = base+0..base+9，rbx rbp rdi rsi r12 r13 r14 r15 = base+10..base+17，
# base = 0x600000000000，18 个值之和 = 0x6C00000000099。
# 这些数字必须和 C++ 侧 examples/abi_switch_smoke.cpp 里的 magic_base 一致：
# 那边会在 B 的上下文里直接读 A 的固定帧逐槽比对，所以两边写错会立刻被抓到。

	.text

# uint64_t abi_switch_probe(void* from_frame, void* to_frame);
# 返回差值和：0 表示 18 个槽全对。
	.globl	abi_switch_probe
	.def	abi_switch_probe;	.scl	2;	.type	32;	.endef
	.p2align 4
abi_switch_probe:
	# 入口 rsp % 16 == 8。264 = 32(shadow) + 160(xmm) + 64(gpr) + 8(对齐垫片)，
	# sub 之后 rsp % 16 == 0，call abi_switch_swap 前后都合法。
	subq	$264, %rsp
	# 备份调用者的 xmm6..xmm15 与 8 个非易失 GPR（本函数是裸的，自己守 ABI）
	movdqu	%xmm6, 32(%rsp)
	movdqu	%xmm7, 48(%rsp)
	movdqu	%xmm8, 64(%rsp)
	movdqu	%xmm9, 80(%rsp)
	movdqu	%xmm10, 96(%rsp)
	movdqu	%xmm11, 112(%rsp)
	movdqu	%xmm12, 128(%rsp)
	movdqu	%xmm13, 144(%rsp)
	movdqu	%xmm14, 160(%rsp)
	movdqu	%xmm15, 176(%rsp)
	movq	%rbx, 192(%rsp)
	movq	%rbp, 200(%rsp)
	movq	%rdi, 208(%rsp)
	movq	%rsi, 216(%rsp)
	movq	%r12, 224(%rsp)
	movq	%r13, 232(%rsp)
	movq	%r14, 240(%rsp)
	movq	%r15, 248(%rsp)
	# 填魔数：xmm6..xmm15 = base+0..base+9
	movabsq	$0x600000000000, %rax
	movq	%rax, %xmm6
	incq	%rax
	movq	%rax, %xmm7
	incq	%rax
	movq	%rax, %xmm8
	incq	%rax
	movq	%rax, %xmm9
	incq	%rax
	movq	%rax, %xmm10
	incq	%rax
	movq	%rax, %xmm11
	incq	%rax
	movq	%rax, %xmm12
	incq	%rax
	movq	%rax, %xmm13
	incq	%rax
	movq	%rax, %xmm14
	incq	%rax
	movq	%rax, %xmm15
	# 接着 8 个 GPR = base+10..base+17
	incq	%rax
	movq	%rax, %rbx
	incq	%rax
	movq	%rax, %rbp
	incq	%rax
	movq	%rax, %rdi
	incq	%rax
	movq	%rax, %rsi
	incq	%rax
	movq	%rax, %r12
	incq	%rax
	movq	%rax, %r13
	incq	%rax
	movq	%rax, %r14
	incq	%rax
	movq	%rax, %r15
	# rcx / rdx 还是 from / to，直接换栈；B 切回来时从这里继续
	call	abi_switch_swap
	# 核对：xmm 低 64 位求和 + GPR 求和，与期望总值相减
	xorl	%r10d, %r10d
	movq	%xmm6, %rax
	addq	%rax, %r10
	movq	%xmm7, %rax
	addq	%rax, %r10
	movq	%xmm8, %rax
	addq	%rax, %r10
	movq	%xmm9, %rax
	addq	%rax, %r10
	movq	%xmm10, %rax
	addq	%rax, %r10
	movq	%xmm11, %rax
	addq	%rax, %r10
	movq	%xmm12, %rax
	addq	%rax, %r10
	movq	%xmm13, %rax
	addq	%rax, %r10
	movq	%xmm14, %rax
	addq	%rax, %r10
	movq	%xmm15, %rax
	addq	%rax, %r10
	xorl	%r11d, %r11d
	movq	%rbx, %rax
	addq	%rax, %r11
	movq	%rbp, %rax
	addq	%rax, %r11
	movq	%rdi, %rax
	addq	%rax, %r11
	movq	%rsi, %rax
	addq	%rax, %r11
	movq	%r12, %rax
	addq	%rax, %r11
	movq	%r13, %rax
	addq	%rax, %r11
	movq	%r14, %rax
	addq	%rax, %r11
	movq	%r15, %rax
	addq	%rax, %r11
	addq	%r11, %r10
	movabsq	$0x6C00000000099, %rax
	subq	%r10, %rax
	# 还原调用者的寄存器
	movdqu	32(%rsp), %xmm6
	movdqu	48(%rsp), %xmm7
	movdqu	64(%rsp), %xmm8
	movdqu	80(%rsp), %xmm9
	movdqu	96(%rsp), %xmm10
	movdqu	112(%rsp), %xmm11
	movdqu	128(%rsp), %xmm12
	movdqu	144(%rsp), %xmm13
	movdqu	160(%rsp), %xmm14
	movdqu	176(%rsp), %xmm15
	movq	192(%rsp), %rbx
	movq	200(%rsp), %rbp
	movq	208(%rsp), %rdi
	movq	216(%rsp), %rsi
	movq	224(%rsp), %r12
	movq	232(%rsp), %r13
	movq	240(%rsp), %r14
	movq	248(%rsp), %r15
	addq	$264, %rsp
	ret

# void abi_switch_trash_and_swap(void* from_frame, void* to_frame);
# 把 18 个非易失寄存器砸成垃圾后直接跳进换栈器。
# 用途：如果 B 自己没碰过这些寄存器，"存了但忘了恢复"也能蒙混过关；主动砸烂以后，
# A 回来只能是它固定帧里那份魔数。B 不会再回来，所以砸坏 B 自己的状态无所谓。
	.globl	abi_switch_trash_and_swap
	.def	abi_switch_trash_and_swap;	.scl	2;	.type	32;	.endef
	.p2align 4
abi_switch_trash_and_swap:
	movabsq	$0x7E7E7E7E7E7E7E7E, %rax
	movq	%rax, %rbx
	movq	%rax, %rbp
	movq	%rax, %rdi
	movq	%rax, %rsi
	movq	%rax, %r12
	movq	%rax, %r13
	movq	%rax, %r14
	movq	%rax, %r15
	movq	%rax, %xmm6
	movq	%rax, %xmm7
	movq	%rax, %xmm8
	movq	%rax, %xmm9
	movq	%rax, %xmm10
	movq	%rax, %xmm11
	movq	%rax, %xmm12
	movq	%rax, %xmm13
	movq	%rax, %xmm14
	movq	%rax, %xmm15
	jmp	abi_switch_swap
