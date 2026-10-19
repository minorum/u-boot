// SPDX-License-Identifier: GPL-2.0-or-later
#include <init.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/bitops.h>

DECLARE_GLOBAL_DATA_PTR;

int dram_init(void)
{
	gd->ram_size = 0x07800000;
	return 0;
}

int dram_init_banksize(void)
{
	gd->dram[0].start = 0x80000000;
	gd->dram[0].size = gd->ram_size;
	return 0;
}

int board_init(void)
{
	gd->bd->bi_arch_number = 0x7de;
	gd->bd->bi_boot_params = 0x85400000;
	setbits_le32((void *)0x402e0000, BIT(11));
	setbits_le32((void *)0x20e00000, BIT(4));
	return 0;
}
