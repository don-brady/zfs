/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 1992, 2010, Oracle and/or its affiliates. All rights reserved.
 */

#ifdef __sparc__

#if defined(__arch64__)
#define	LABEL_VAL_INCR	8
#define	RETPTRSIZE	0	/* no struct/union return pointer */
#else
#define	LABEL_VAL_INCR	4
#define	RETPTRSIZE	4	/* size of struct/union return pointer */
#endif /* __arch64__ */

#define	WINDOWSIZE	(16*LABEL_VAL_INCR)	/* size of window save area */
#define	ARGPUSHSIZE	(6*LABEL_VAL_INCR)	/* size of arg dump area */
#define	MINFRAME	(WINDOWSIZE + ARGPUSHSIZE + RETPTRSIZE)	/* min frame */

#define	STACK_ALIGN	(2 * LABEL_VAL_INCR)
#define	SA(x)		(((x)+(STACK_ALIGN-1)) & ~(STACK_ALIGN-1))

#define	L_PC	(0 * LABEL_VAL_INCR)
#define	L_SP	(1 * LABEL_VAL_INCR)

#define ENTRY(x) \
	.section ".text"; \
	.align	4; \
	.global	x; \
	.type	x, #function; \
x:

#define SET_SIZE(x) \
	.size	x, (.-x)

/*
 * Setjmp and longjmp implement non-local gotos using state vectors
 * type label_t.
 */

	ENTRY(setjmp)
	stn	%o7, [%o0 + L_PC]	! save return address
	stn	%sp, [%o0 + L_SP]	! save stack ptr
	retl
	clr	%o0			! return 0
	SET_SIZE(setjmp)

	ENTRY(longjmp)
	!
	! The following save is required so that an extra register
	! window is flushed.  Flushw flushes nwindows-2
	! register windows.  If setjmp and longjmp are called from
	! within the same window, that window will not get pushed
	! out onto the stack without the extra save below.  Tail call
	! optimization can lead to callers of longjmp executing
	! from a window that could be the same as the setjmp,
	! thus the need for the following save.
	!
	save	%sp, -SA(MINFRAME), %sp
	flushw				! flush all but this window
	ldn	[%i0 + L_PC], %i7	! restore return addr
	ldn	[%i0 + L_SP], %fp	! restore sp for dest on foreign stack
	ret				! return 1
	restore	%g0, 1, %o0		! takes underflow, switches stacks
	SET_SIZE(longjmp)

#ifdef __ELF__
.section .note.GNU-stack,"",%progbits
#endif

#endif /* __sparc__ */
