// SPDX-License-Identifier: GPL-2.0-only
#include <config.h>
#include <cpu_func.h>
#include <init.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/sizes.h>

DECLARE_GLOBAL_DATA_PTR;

int board_early_init_f(void)
{
	void __iomem *mode = (void *)0xc0000000;

	writew((readw(mode) & 0xfe) | 0x2200, mode);
	return 0;
}

int dram_init(void)
{
	gd->ram_size = SZ_16M;
	return 0;
}

int board_init(void)
{
	gd->bd->bi_boot_params = CFG_SYS_SDRAM_BASE + 0x100;
	return 0;
}

void reset_cpu(void)
{
	writew(0x1209, (void *)0xc0000014);
	for (;;)
		;
}
