/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6577_PHY_H
#define __MT6577_PHY_H

static inline void mt6577_phy_recover(void __iomem *phy)
{
	clrbits_8(phy + 0x6b, 0x04);
	clrbits_8(phy + 0x6e, 0x01);
	clrbits_8(phy + 0x68, 0xf4);
	clrbits_8(phy + 0x69, 0x3c);
	clrbits_8(phy + 0x6a, 0xbe);
	clrbits_8(phy + 0x1c, 0x80);
	setbits_8(phy + 0x1b, 0x08);
	udelay(800);
}

#endif
