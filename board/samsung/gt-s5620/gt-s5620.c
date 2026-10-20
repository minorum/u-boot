// SPDX-License-Identifier: GPL-2.0+

#include <config.h>
#include <env.h>
#include <init.h>
#include <stdio.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

int checkboard(void)
{
	u32 midr;

	asm volatile("mrc p15, 0, %0, c0, c0, 0" : "=r" (midr));
	printf("CPU MIDR: %08x\n", midr);
	return env_set_hex("cpu_midr", midr);
}

int board_late_init(void)
{
	puts("USB: waiting for host connection\n");
	if (env_set("stdout", "vidconsole,usbacm")) {
		puts("USB console unavailable\n");
		return 0;
	}
	if (env_set("stderr", "vidconsole,usbacm") ||
	    env_set("stdin", "usbacm"))
		puts("USB console assignment failed\n");
	return 0;
}

int board_init(void)
{
	return 0;
}

int dram_init(void)
{
	gd->ram_size = 0x08000000;
	gd->dram[0].start = CFG_SYS_SDRAM_BASE;
	gd->dram[0].size = gd->ram_size;
	return 0;
}

phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	return 0x87f00000;
}
