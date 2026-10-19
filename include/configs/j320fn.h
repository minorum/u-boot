/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __CONFIG_J320FN_H
#define __CONFIG_J320FN_H

#define CFG_SYS_SDRAM_BASE 0x80000000
#ifdef CONFIG_TARGET_SAMSUNG_J320F
#define J320F_BOOT_ENV "boot_targets=mmc0:27\0"
#else
#define J320F_BOOT_ENV ""
#endif
#define CFG_EXTRA_ENV_SETTINGS \
	J320F_BOOT_ENV \
	"kernel_addr_r=0x80008000\0" \
	"pxefile_addr_r=0x85300000\0" \
	"fdt_addr_r=0x85400000\0" \
	"ramdisk_addr_r=0x85500000\0" \
	"fdt_high=0x877fffff\0" \
	"initrd_high=0x877fffff\0"

#endif
