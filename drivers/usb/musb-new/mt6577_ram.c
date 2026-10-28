// SPDX-License-Identifier: GPL-2.0-only
#include <dm.h>
#include <linux/delay.h>
#include "musb_uboot.h"

#define MT6577_SWRST 0x74
#define MT6577_L1INTM 0xa4

struct mt6577_glue {
	struct device dev;
	struct musb *musb;
};

static irqreturn_t mt6577_interrupt(int irq, void *data)
{
	struct musb *musb = data;
	void __iomem *base = musb->mregs;
	irqreturn_t ret = IRQ_NONE;

	musb->int_usb = musb_readb(base, MUSB_INTRUSB) &
			musb_readb(base, MUSB_INTRUSBE);
	musb->int_tx = musb_readw(base, MUSB_INTRTX) &
		       musb_readw(base, MUSB_INTRTXE);
	musb->int_rx = musb_readw(base, MUSB_INTRRX) &
		       musb_readw(base, MUSB_INTRRXE);
	mb();
	musb_writeb(base, MUSB_INTRUSB, musb->int_usb);
	musb_writew(base, MUSB_INTRTX, musb->int_tx);
	musb_writew(base, MUSB_INTRRX, musb->int_rx);
	if (musb->int_usb & MUSB_INTR_RESET) {
		musb_g_reset(musb);
		musb->int_tx &= 1;
		musb->int_rx = 0;
		ret = IRQ_HANDLED;
	} else if (musb->int_usb & MUSB_INTR_DISCONNECT) {
		musb_g_disconnect(musb);
		musb->int_tx = 0;
		musb->int_rx = 0;
		ret = IRQ_HANDLED;
	}
	musb->int_usb &= ~(MUSB_INTR_RESET | MUSB_INTR_DISCONNECT);

	if (musb->int_usb || musb->int_tx || musb->int_rx)
		ret |= musb_interrupt(musb);
	return ret;
}

static int mt6577_init(struct musb *musb)
{
	void __iomem *base = musb->mregs;

	/* shortcut: DA owns PHY calibration, replace for cold boot */
	musb_writeb(base, MUSB_POWER, 0);
	mdelay(20);
	musb_writew(base, MT6577_SWRST,
		    musb_readw(base, MT6577_SWRST) | 3);
	musb_writel(base, MT6577_L1INTM, 7);
	musb->isr = mt6577_interrupt;
	return 0;
}

static const struct musb_platform_ops mt6577_ops = {
	.init = mt6577_init,
};

static struct musb_fifo_cfg mt6577_fifo[] = {
	MUSB_EP_FIFO_SINGLE(1, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(1, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(2, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(2, FIFO_RX, 512),
};

static struct musb_hdrc_config mt6577_config = {
	.fifo_cfg = mt6577_fifo,
	.fifo_cfg_size = ARRAY_SIZE(mt6577_fifo),
	.dyn_fifo = true,
	.num_eps = 3,
	.ram_bits = 11,
};

static int mt6577_probe(struct udevice *dev)
{
	struct mt6577_glue *glue = dev_get_priv(dev);
	struct musb_hdrc_platform_data pdata = {
		.mode = MUSB_PERIPHERAL,
		.config = &mt6577_config,
		.platform_ops = &mt6577_ops,
	};
	void *base = dev_read_addr_ptr(dev);
	int ret;

	if (!base)
		return -EINVAL;
	glue->musb = musb_init_controller(&pdata, &glue->dev, base);
	if (!glue->musb)
		return -EIO;
	ret = usb_add_gadget_udc(&glue->dev, &glue->musb->g);
	if (ret) {
		musb_stop(glue->musb);
		free(glue->musb);
	}
	return ret;
}

static int mt6577_remove(struct udevice *dev)
{
	struct mt6577_glue *glue = dev_get_priv(dev);

	usb_del_gadget_udc(&glue->musb->g);
	musb_stop(glue->musb);
	free(glue->musb);
	return 0;
}

static int mt6577_handle_interrupts(struct udevice *dev)
{
	struct mt6577_glue *glue = dev_get_priv(dev);

	mt6577_interrupt(0, glue->musb);
	return 0;
}

static const struct usb_gadget_generic_ops mt6577_gadget_ops = {
	.handle_interrupts = mt6577_handle_interrupts,
};

static const struct udevice_id mt6577_ids[] = {
	{ .compatible = "mediatek,mt6577-musb-ram" },
	{ }
};

U_BOOT_DRIVER(mt6577_musb_ram) = {
	.name = "mt6577-musb-ram",
	.id = UCLASS_USB_GADGET_GENERIC,
	.of_match = mt6577_ids,
	.probe = mt6577_probe,
	.remove = mt6577_remove,
	.ops = &mt6577_gadget_ops,
	.priv_auto = sizeof(struct mt6577_glue),
};
