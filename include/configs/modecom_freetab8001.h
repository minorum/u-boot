/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MODECOM_FREETAB8001_H
#define __MODECOM_FREETAB8001_H

#define CFG_SYS_SDRAM_BASE 0x10000000
#define CFG_EXTRA_ENV_SETTINGS \
	"stdin=usbacm\0" \
	"stdout=usbacm\0" \
	"stderr=usbacm\0"

#endif
