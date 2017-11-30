/*-
 * Copyright (c) 2001 Jake Burkholder.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define	_JB_FP		0
#define	_JB_PC		1
#define	_JB_SP		2

#define	ENTRY(x)		\
	.text ;			\
	.align 32 ;		\
	.globl	x ;		\
	.type	x,@function ;	\
x:

#define	END(x)			\
	.size x, . - x

#define	PANIC(msg, r1)		\
	.sect	.rodata ;	\
9:	.asciz	msg ;		\
	.previous ;		\
	set	9b, %o0 ;	\
	call	panic ;		\
	 nop


ENTRY(longjmp)
	set	1, %g3
	movrz	%o1, %o1, %g3
	mov	%o0, %g1
	ldx	[%g1 + _JB_FP], %g2
1:	cmp	%fp, %g2
	bl,a,pt	%xcc, 1b
	 restore
	bne,pn	%xcc, 2f
	 ldx	[%g1 + _JB_SP], %o2
	cmp	%o2, %sp
	blt,pn	%xcc, 2f
	 movge	%xcc, %o2, %sp
	ldx	[%g1 + _JB_PC], %o7
	retl
	 mov	%g3, %o0
2:	PANIC("longjmp botch", %l1)
END(longjmp)

ENTRY(setjmp)
	stx	%sp, [%o0 + _JB_SP]
	stx	%o7, [%o0 + _JB_PC]
	stx	%fp, [%o0 + _JB_FP]
	retl
	 clr	%o0
END(setjmp)

#ifdef __ELF__
.section .note.GNU-stack,"",%progbits
#endif
