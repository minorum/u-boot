// SPDX-License-Identifier: GPL-2.0+

#include <errno.h>
#include <stdio.h>
#include <usb.h>
#include <usb/dwc2_udc.h>
#include <asm/io.h>
#include <linux/iopoll.h>

#define USB_CORE	0x08200000
#define USB_PHY		0x08280000
#define USB_RESET	0x088800a4

static bool usb_attempted;
static int usb_result;

static struct dwc2_plat_otg_data usb_data = {
	.regs_otg = USB_CORE,
	.rx_fifo_sz = 512,
	.np_tx_fifo_sz = 128,
	.tx_fifo_sz = 128,
};

static int gt_s5620_usb_init(void)
{
	void *core = (void *)USB_CORE;
	u32 id, hw2, hw4, depth, endpoints, status;
	int ret;

	setbits_le32((void *)USB_PHY, 0x8000);
	clrbits_le32((void *)USB_PHY, 0xc00);
	setbits_le32((void *)USB_PHY, 0x18c00);
	writel(0x4000007f, (void *)USB_RESET);
	writel(0x4000005f, (void *)USB_RESET);
	writel(0x4000007f, (void *)USB_RESET);
	writel(0, core + 0xe00);

	id = readl(core + 0x40);
	if (id >> 16 != 0x4f54) {
		printf("USB: unexpected core ID %08x\n", id);
		return -ENODEV;
	}
	hw2 = readl(core + 0x48);
	depth = readl(core + 0x4c) >> 16;
	hw4 = readl(core + 0x50);
	endpoints = hw4 >> 26 & 0xf;
	printf("USB: core %08x, FIFO %u, IN endpoints %u\n", id, depth, endpoints);
	if (!(hw2 & BIT(19)) || ((hw2 >> 3) & 3) != 2 ||
	    ((hw2 >> 10) & 0xf) < 3 || !(hw4 & BIT(25)) || endpoints < 3)
		return -EOPNOTSUPP;
	if (depth < usb_data.rx_fifo_sz + usb_data.np_tx_fifo_sz +
	    endpoints * usb_data.tx_fifo_sz)
		return -ENOSPC;
	ret = readl_poll_timeout(core + 0x10, status, status & BIT(31), 100000);
	if (ret)
		return ret;
	usb_data.usb_gusbcfg = readl(core + 0x0c) | 0x40002400;
	writel(usb_data.usb_gusbcfg, core + 0x0c);
	return dwc2_udc_probe(&usb_data);
}

int board_usb_init(int index, enum usb_init_type init)
{
	if (index || init != USB_INIT_DEVICE)
		return -EINVAL;
	if (!usb_attempted) {
		usb_attempted = true;
		usb_result = gt_s5620_usb_init();
	}
	return usb_result;
}
