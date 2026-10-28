// SPDX-License-Identifier: GPL-2.0-only
#include <config.h>
#include <cpu_func.h>
#include <errno.h>
#include <efi_loader.h>
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

#ifdef CONFIG_EFI_LOADER
static void __iomem *efi_wdt_base __efi_runtime_data;

void __efi_runtime EFIAPI efi_reset_system(enum efi_reset_type reset_type,
					   efi_status_t reset_status,
					   unsigned long data_size, void *reset_data)
{
	if (reset_type == EFI_RESET_COLD || reset_type == EFI_RESET_WARM ||
	    reset_type == EFI_RESET_PLATFORM_SPECIFIC)
		writew(0x1209, efi_wdt_base + 0x14);
	for (;;)
		;
}

efi_status_t efi_reset_system_init(void)
{
	efi_wdt_base = (void *)0xc0000000;
	return efi_add_runtime_mmio(&efi_wdt_base, SZ_4K);
}
#endif
