/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>

	.arm
	.text
ENTRY(save_boot_params)
	adr r12, atags_offset
	ldr r3, atags_offset
	add r12, r12, r3
	str r2, [r12]
	b save_boot_params_ret
ENDPROC(save_boot_params)

atags_offset:
	.long modecom_boot_atags - .

	.section .data
	.align 2
	.global modecom_boot_atags
modecom_boot_atags:
	.word 0
END(modecom_boot_atags)
