// SPDX-License-Identifier: GPL-2.0-only
#include <config.h>
#include <cpu_func.h>
#include <errno.h>
#include <efi_loader.h>
#include <fdt_support.h>
#include <init.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/sections.h>
#include <linux/sizes.h>
#include <linux/build_bug.h>

#include "atags.h"

DECLARE_GLOBAL_DATA_PTR;

static struct modecom_ram ram __section(".data");

static_assert(CONFIG_NR_DRAM_BANKS == MODECOM_RAM_BANKS);

int board_early_init_f(void)
{
	void __iomem *mode = (void *)0xc0000000;

	writew((readw(mode) & 0xfe) | 0x2200, mode);
	return 0;
}

int dram_init(void)
{
	u32 initial_size = CONFIG_STACK_SIZE + CONFIG_SYS_MALLOC_F_LEN + sizeof(*gd);
	unsigned int i;
	int ret;

	if (modecom_boot_atags) {
		if (modecom_boot_atags != 0x01600100)
			return -EINVAL;
		ret = modecom_parse_atags((const u32 *)(ulong)modecom_boot_atags,
					  1024, &ram);
		if (ret)
			return ret;
	} else {
		ram = (struct modecom_ram){
			.start = {CFG_SYS_SDRAM_BASE},
			.size = {SZ_64M},
			.count = 1,
		};
	}
	if (!modecom_ram_contains(&ram, (ulong)_start, gd->mon_len) ||
	    !modecom_ram_contains(&ram, (ulong)gd->fdt_blob, fdt_totalsize(gd->fdt_blob)) ||
	    !modecom_ram_contains(&ram, CONFIG_CUSTOM_SYS_INIT_SP_ADDR - initial_size,
				 initial_size))
		return -EINVAL;
	gd->ram_size = 0;
	for (i = 0; i < ram.count; i++)
		gd->ram_size += ram.size[i];
	return 0;
}

int dram_init_banksize(void)
{
	unsigned int i;

	gd->ram_base = ram.start[0];
	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		gd->dram[i].start = ram.start[i];
		gd->dram[i].size = ram.size[i];
	}
	return 0;
}

int board_init(void)
{
	gd->bd->bi_boot_params = gd->ram_base + 0x100;
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

int ft_board_setup(void *blob, struct bd_info *bd)
{
	char name[32];
	int ret, node;

	ret = board_fix_fdt(blob);
	if (ret)
		return ret;
	node = fdt_path_offset(blob, "/memory");
	if (node < 0)
		return node;
	snprintf(name, sizeof(name), "memory@%llx", (unsigned long long)gd->ram_base);
	return fdt_set_name(blob, node, name);
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
