// SPDX-License-Identifier: GPL-2.0-only
#include <config.h>
#include <cpu_func.h>
#include <errno.h>
#include <fdt_support.h>
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
	gd->ram_size = SZ_64M;
	return 0;
}

int board_init(void)
{
	gd->bd->bi_boot_params = CFG_SYS_SDRAM_BASE + 0x100;
	return 0;
}

int board_fix_fdt(void *blob)
{
	static const u32 rates[] = {197000001, 197000000, 192000000};
	u32 source = readl((void *)0xc100000c) & 3;
	int node;

	if (source >= ARRAY_SIZE(rates))
		return -EINVAL;
	node = fdt_path_offset(blob, "/clock-msdc");
	if (node < 0)
		return node;
	return fdt_setprop_u32(blob, node, "clock-frequency", rates[source]);
}

void reset_cpu(void)
{
	writew(0x1209, (void *)0xc0000014);
	for (;;)
		;
}
