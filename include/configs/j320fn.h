/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __CONFIG_J320FN_H
#define __CONFIG_J320FN_H

#define CFG_SYS_SDRAM_BASE 0x80000000
#define CFG_EXTRA_ENV_SETTINGS \
	"kernel_addr_r=0x80008000\0" \
	"pxefile_addr_r=0x85300000\0" \
	"fdt_addr_r=0x85400000\0" \
	"ramdisk_addr_r=0x85500000\0" \
	"fdt_high=0x877fffff\0" \
	"initrd_high=0x877fffff\0"

#endif
