// SPDX-License-Identifier: GPL-2.0+
#include <env.h>
#include <fdtdec.h>
#include <init.h>
#include <lmb.h>
#include <malloc.h>
#include <asm/global_data.h>
#include <asm/sections.h>
#include <asm/system.h>
#include <linux/errno.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include "memory.h"

DECLARE_GLOBAL_DATA_PTR;

static const void *loader_fdt(void)
{
	const void *fdt = (void *)get_prev_bl_fdt_addr();

	if (!fdt || fdt_check_header(fdt) ||
	    fdt_check_full(fdt, fdt_totalsize(fdt)))
		panic("Missing loader FDT\n");
	return fdt;
}

int dram_init(void)
{
	const void *fdt = loader_fdt();
	fdt_size_t size;
	fdt_addr_t addr;
	int node = -1, bank = 0, index;

	gd->ram_size = 0;
	gd->ram_base = ~0UL;
	while ((node = fdt_node_offset_by_prop_value(fdt, node, "device_type",
						   "memory", 7)) >= 0) {
		for (index = 0; ; index++) {
			addr = fdtdec_get_addr_size_auto_parent(fdt, 0, node,
							       "reg", index, &size, false);
			if (addr == FDT_ADDR_T_NONE)
				break;
			if (!size || addr + size < addr ||
			    gd->ram_size + size < gd->ram_size)
				return -EINVAL;
			if (bank == CONFIG_NR_DRAM_BANKS)
				return -ENOSPC;
			gd->dram[bank].start = addr;
			gd->dram[bank++].size = size;
			gd->ram_base = min_t(ulong, gd->ram_base, addr);
			gd->ram_size += size;
		}
	}
	return bank ? 0 : -EINVAL;
}

int dram_init_banksize(void)
{
	return 0;
}

static void store_reservation(struct begonia_region *regions,
			      unsigned int *count, u64 start, u64 size)
{
	if (!size)
		return;
	if (regions) {
		regions[*count].start = start;
		regions[*count].size = size;
	}
	(*count)++;
}

static unsigned int loader_reservations(struct begonia_region *regions)
{
	const void *fdt = loader_fdt();
	const fdt32_t *prop;
	fdt_size_t size;
	fdt_addr_t addr;
	u64 start, end;
	unsigned int count = 0;
	int reserved, node, index;

	store_reservation(regions, &count, (uintptr_t)fdt, fdt_totalsize(fdt));
	node = fdt_path_offset(fdt, "/chosen");
	if (node >= 0) {
		prop = fdt_getprop(fdt, node, "linux,initrd-start", &index);
		if (prop) {
			if (index != 4 && index != 8)
				panic("Invalid loader initrd address\n");
			start = fdtdec_get_number(prop, index / 4);
			prop = fdt_getprop(fdt, node, "linux,initrd-end", &index);
			if (!prop || (index != 4 && index != 8))
				panic("Invalid loader initrd end\n");
			end = fdtdec_get_number(prop, index / 4);
			if (end < start)
				panic("Invalid loader initrd range\n");
			store_reservation(regions, &count, start, end - start);
		}
	}
	for (index = 0; index < fdt_num_mem_rsv(fdt); index++) {
		if (fdt_get_mem_rsv(fdt, index, &start, &end))
			panic("Invalid loader reserve map\n");
		store_reservation(regions, &count, start, end);
	}
	reserved = fdt_path_offset(fdt, "/reserved-memory");
	if (reserved >= 0) {
		fdt_for_each_subnode(node, fdt, reserved) {
			if (!fdtdec_get_is_enabled(fdt, node))
				continue;
			for (index = 0; ; index++) {
				addr = fdtdec_get_addr_size_auto_parent(fdt, reserved,
								       node, "reg", index,
								       &size, false);
				if (addr == FDT_ADDR_T_NONE)
					break;
				store_reservation(regions, &count, addr, size);
			}
		}
	}
	return count;
}

static int walk_free_memory(int (*visit)(u64 start, u64 size, void *arg), void *arg)
{
	struct begonia_region banks[CONFIG_NR_DRAM_BANKS], *reserved;
	unsigned int count, nbanks = 0, i;
	int ret;

	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		if (!gd->dram[i].size)
			continue;
		banks[nbanks].start = gd->dram[i].start;
		banks[nbanks++].size = gd->dram[i].size;
	}
	count = loader_reservations(NULL);
	reserved = malloc(count * sizeof(*reserved));
	if (!reserved)
		return -ENOMEM;
	loader_reservations(reserved);
	ret = begonia_free_memory(banks, nbanks, reserved, count, visit, arg);
	free(reserved);
	return ret;
}

static int select_relocation_window(u64 start, u64 size, void *arg)
{
	if (start <= (uintptr_t)_start && (uintptr_t)_end <= start + size)
		*(phys_addr_t *)arg = start + size;
	return 0;
}

phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	phys_addr_t top = 0;

	if (walk_free_memory(select_relocation_window, &top) ||
	    top <= (uintptr_t)_end + CONFIG_SYS_MALLOC_LEN + SZ_1M + total_size)
		panic("No safe relocation window\n");
	return top;
}

static int add_lmb_memory(u64 start, u64 size, void *arg)
{
	return lmb_add(start, size);
}

void lmb_arch_add_memory(void)
{
	if (walk_free_memory(add_lmb_memory, NULL))
		panic("Cannot initialize safe memory map\n");
}

int board_init(void)
{
	return 0;
}

int print_cpuinfo(void)
{
	puts("CPU:   MediaTek MT6785\n");
	return 0;
}

int board_late_init(void)
{
	return env_set_hex("fdt_addr", (uintptr_t)loader_fdt());
}

void reset_cpu(void)
{
	psci_system_reset();
}
