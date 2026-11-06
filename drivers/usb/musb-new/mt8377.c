// SPDX-License-Identifier: GPL-2.0-only
#include <dm.h>
#include <clk.h>
#include <cyclic.h>
#include <generic-phy.h>
#include <linux/delay.h>
#include <limits.h>
#include <power/charger.h>
#include "musb_uboot.h"

#define MT8377_SWRST 0x74
#define MT8377_L1INTM 0xa4

struct mt8377_glue {
	struct device dev;
	struct musb *musb;
	struct clk *bus;
	struct phy phy;
	struct udevice *charger;
	struct cyclic_info budget_poll;
	unsigned int budget_ma;
	u8 budget_events;
	bool budget_pending;
	bool budget_busy;
	bool budget_registered;
	bool stopping;
	bool bus_on;
	bool initialized;
	bool powered;
};

static int mt8377_apply_budget(struct mt8377_glue *glue)
{
	unsigned int ma;
	int ret;

	if (!IS_ENABLED(CONFIG_DM_CHARGER) || !glue->charger ||
	    !glue->budget_pending)
		return 0;
	if (!device_active(glue->charger))
		return -ENODEV;
	ret = charger_set_usb_current(glue->charger, glue->budget_ma * 1000);
	if (ret && ret != -ENOSYS)
		return ret;
	if (glue->budget_busy)
		return -EBUSY;
	glue->budget_busy = true;
	ma = glue->budget_ma;
	ret = charger_set_input_current(glue->charger, ma * 1000);
	if (!ret && ma == glue->budget_ma)
		glue->budget_pending = false;
	glue->budget_busy = false;
	return ret;
}

static int mt8377_vbus_draw(struct musb *musb, unsigned int ma)
{
	struct mt8377_glue *glue = container_of(musb->controller,
						    struct mt8377_glue, dev);

	if (ma > INT_MAX / 1000)
		return -EINVAL;
	glue->budget_ma = glue->stopping ? 0 : ma;
	glue->budget_pending = true;
	if (IS_ENABLED(CONFIG_DM_CHARGER) && glue->charger &&
	    device_active(glue->charger)) {
		int ret = charger_set_usb_current(glue->charger,
						 glue->budget_ma * 1000);

		if (ret && ret != -ENOSYS)
			return ret;
	}
	return 0;
}

static int mt8377_usb_event_poll(void *data)
{
	struct mt8377_glue *glue = data;
	u8 events, fresh;
	unsigned int ma;

	if (!glue->musb || !glue->bus_on || glue->stopping)
		return 0;
	events = musb_readb(glue->musb->mregs, MUSB_INTRUSB) &
		 (MUSB_INTR_SUSPEND | MUSB_INTR_RESET | MUSB_INTR_DISCONNECT);
	fresh = events & ~glue->budget_events;
	glue->budget_events = events;
	if (!fresh)
		return 0;
	ma = events & MUSB_INTR_DISCONNECT ? 0 :
		(events & MUSB_INTR_SUSPEND ? 2 : 100);
	return mt8377_vbus_draw(glue->musb, ma);
}

static void mt8377_budget_poll(struct cyclic_info *cyclic)
{
	struct mt8377_glue *glue = container_of(cyclic, struct mt8377_glue,
						    budget_poll);

	if (mt8377_usb_event_poll(glue))
		return;
	mt8377_apply_budget(glue);
}

static volatile u32 mt8377_reset_trace[6];

static int mt8377_resources_off(struct mt8377_glue *glue)
{
	int ret;

	if (glue->powered) {
		ret = generic_phy_power_off(&glue->phy);
		if (ret)
			return ret;
		glue->powered = false;
	}
	if (glue->initialized) {
		ret = generic_phy_exit(&glue->phy);
		if (ret)
			return ret;
		glue->initialized = false;
	}
	if (glue->bus_on) {
		ret = clk_disable(glue->bus);
		if (ret)
			return ret;
		glue->bus_on = false;
	}
	return 0;
}

static int mt8377_resources_on(struct mt8377_glue *glue)
{
	int ret, cleanup;

	if (clk_get_rate(glue->bus) != 61000000)
		return -EINVAL;
	ret = generic_phy_set_mode(&glue->phy, PHY_MODE_USB_DEVICE, 0);
	if (ret)
		return ret;
	ret = clk_enable(glue->bus);
	if (ret)
		return ret;
	glue->bus_on = true;
	ret = generic_phy_init(&glue->phy);
	if (ret)
		goto cleanup;
	glue->initialized = true;
	ret = generic_phy_power_on(&glue->phy);
	if (ret)
		goto cleanup;
	glue->powered = true;
	return 0;
cleanup:
	cleanup = mt8377_resources_off(glue);
	return cleanup ? cleanup : ret;
}

static irqreturn_t mt8377_interrupt(int irq, void *data)
{
	struct musb *musb = data;
	struct mt8377_glue *glue = container_of(musb->controller,
						    struct mt8377_glue, dev);
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
	glue->budget_events &= ~musb->int_usb;
	musb_writew(base, MUSB_INTRTX, musb->int_tx);
	musb_writew(base, MUSB_INTRRX, musb->int_rx);
	if (musb->int_usb & MUSB_INTR_DISCONNECT) {
		musb_g_disconnect(musb);
		musb->int_usb = 0;
		musb->int_tx = 0;
		musb->int_rx = 0;
		ret = IRQ_HANDLED;
	} else if (musb->int_usb & MUSB_INTR_RESET) {
		mt8377_reset_trace[0]++;
		mt8377_reset_trace[1] = musb_readb(base, MUSB_FADDR);
		mt8377_reset_trace[3] = musb_readb(base, MUSB_POWER);
		mt8377_reset_trace[4] = musb_readb(base, MUSB_DEVCTL);
		musb_writeb(base, MUSB_FADDR, 0);
		mt8377_reset_trace[2] = musb_readb(base, MUSB_FADDR);
		musb_g_reset(musb);
		musb_ep_select(base, 0);
		mt8377_reset_trace[5] = musb_readw(musb->endpoints[0].regs,
						  MUSB_CSR0);
		musb->int_tx &= 1;
		musb->int_rx = 0;
		ret = IRQ_HANDLED;
	}
	musb->int_usb &= ~(MUSB_INTR_RESET | MUSB_INTR_DISCONNECT);

	if (musb->int_usb || musb->int_tx || musb->int_rx)
		ret |= musb_interrupt(musb);
	return ret;
}

static int mt8377_init(struct musb *musb)
{
	void __iomem *base = musb->mregs;

	musb_writeb(base, MUSB_POWER, 0);
	mdelay(20);
	musb_writew(base, MT8377_SWRST,
		    musb_readw(base, MT8377_SWRST) | 3);
	musb_writel(base, MT8377_L1INTM, 7);
	musb->isr = mt8377_interrupt;
	return 0;
}

static const struct musb_platform_ops mt8377_ops = {
	.init = mt8377_init,
	.vbus_draw = mt8377_vbus_draw,
};

static struct musb_fifo_cfg mt8377_fifo[] = {
	MUSB_EP_FIFO_SINGLE(1, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(1, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(2, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(2, FIFO_RX, 512),
};

static struct musb_hdrc_config mt8377_config = {
	.fifo_cfg = mt8377_fifo,
	.fifo_cfg_size = ARRAY_SIZE(mt8377_fifo),
	.dyn_fifo = true,
	.num_eps = 3,
	.ram_bits = 11,
};

static int mt8377_probe(struct udevice *dev)
{
	struct mt8377_glue *glue = dev_get_priv(dev);
	struct musb_hdrc_platform_data pdata = {
		.mode = MUSB_PERIPHERAL,
		.config = &mt8377_config,
		.platform_ops = &mt8377_ops,
	};
	void *base = dev_read_addr_ptr(dev);
	int ret, cleanup;

	if (!base)
		return -EINVAL;
	glue->bus = devm_clk_get(dev, "bus");
	if (IS_ERR(glue->bus))
		return PTR_ERR(glue->bus);
	ret = generic_phy_get_by_name(dev, "usb2-phy", &glue->phy);
	if (ret)
		return ret;
	if (dev_read_prop(dev, "power-supplies", NULL)) {
		if (!IS_ENABLED(CONFIG_DM_CHARGER))
			return -ENODEV;
		ret = uclass_get_device_by_phandle(UCLASS_CHARGER, dev,
						   "power-supplies", &glue->charger);
		if (ret)
			return ret;
		glue->budget_ma = 0;
		glue->budget_pending = true;
		ret = mt8377_apply_budget(glue);
		if (ret)
			return ret;
	}
	ret = mt8377_resources_on(glue);
	if (ret)
		return ret;
	glue->musb = musb_init_controller(&pdata, &glue->dev, base);
	if (!glue->musb) {
		ret = -EIO;
		goto disable;
	}
	ret = usb_add_gadget_udc(&glue->dev, &glue->musb->g);
	if (ret)
		goto free_musb;
	if (IS_ENABLED(CONFIG_DM_CHARGER) && glue->charger) {
		ret = charger_set_usb_event_poll(glue->charger,
						mt8377_usb_event_poll, glue);
		if (ret && ret != -ENOSYS) {
			charger_set_usb_event_poll(glue->charger, NULL, NULL);
			usb_del_gadget_udc(&glue->musb->g);
			goto free_musb;
		}
		cyclic_register(&glue->budget_poll, mt8377_budget_poll, 1000,
				"mt8377-usb-budget");
		glue->budget_registered = true;
	}
	return 0;
free_musb:
	musb_stop(glue->musb);
	free(glue->musb);
	glue->musb = NULL;
disable:
	cleanup = mt8377_resources_off(glue);
	return cleanup ? cleanup : ret;
}

static int mt8377_remove(struct udevice *dev)
{
	struct mt8377_glue *glue = dev_get_priv(dev);
	int ret, cleanup;

	if (glue->budget_busy)
		return -EBUSY;
	if (IS_ENABLED(CONFIG_DM_CHARGER) && glue->charger &&
	    device_active(glue->charger)) {
		ret = charger_set_usb_event_poll(glue->charger, NULL, NULL);
		if (ret && ret != -ENOSYS)
			return ret;
	}
	glue->stopping = true;
	if (glue->budget_registered) {
		cyclic_unregister(&glue->budget_poll);
		glue->budget_registered = false;
	}

	if (glue->musb) {
		usb_del_gadget_udc(&glue->musb->g);
		musb_stop(glue->musb);
		free(glue->musb);
		glue->musb = NULL;
	}
	glue->budget_ma = 0;
	glue->budget_pending = true;
	ret = mt8377_apply_budget(glue);
	if (ret == -ENODEV)
		ret = 0;
	cleanup = mt8377_resources_off(glue);
	return ret ? ret : cleanup;
}

static int mt8377_handle_interrupts(struct udevice *dev)
{
	struct mt8377_glue *glue = dev_get_priv(dev);

	if (!glue->musb)
		return -ENODEV;
	mt8377_interrupt(0, glue->musb);
	mt8377_apply_budget(glue);
	return 0;
}

static const struct usb_gadget_generic_ops mt8377_gadget_ops = {
	.handle_interrupts = mt8377_handle_interrupts,
};

static const struct udevice_id mt8377_ids[] = {
	{ .compatible = "mediatek,mt8377-musb" },
	{ .compatible = "mediatek,mt6577-musb" },
	{ }
};

U_BOOT_DRIVER(mt8377_musb) = {
	.name = "mt8377-musb",
	.id = UCLASS_USB_GADGET_GENERIC,
	.of_match = mt8377_ids,
	.probe = mt8377_probe,
	.remove = mt8377_remove,
	.ops = &mt8377_gadget_ops,
	.priv_auto = sizeof(struct mt8377_glue),
	.flags = DM_FLAG_OS_PREPARE,
};
