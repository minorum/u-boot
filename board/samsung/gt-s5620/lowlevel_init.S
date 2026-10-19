/* SPDX-License-Identifier: GPL-2.0+ */

#include <linux/linkage.h>

ENTRY(lowlevel_init)
	ldr	r0, =0x0800000f
	mcr	p15, 0, r0, c15, c2, 4
	ldr	r0, =0x08700015
	mcr	p15, 0, r0, c9, c1, 0
	ldr	r0, =0xffff0015
	mcr	p15, 0, r0, c9, c1, 1
	mrc	p15, 0, r0, c1, c0, 0
	orr	r0, r0, #0x2000
	mcr	p15, 0, r0, c1, c0, 0
	bx	lr
ENDPROC(lowlevel_init)
