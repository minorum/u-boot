// SPDX-License-Identifier: GPL-2.0-only
#include <backlight.h>
#include <dm.h>
#include <panel.h>
#include <asm/gpio.h>
#include <linux/delay.h>

struct freetab_panel {
	struct gpio_desc controls[6];
	unsigned int enabled;
	struct udevice *backlight;
};

static int freetab_power_down(struct freetab_panel *ctx)
{
	static const unsigned int order[] = { 2, 5, 4, 3, 1 };
	static const unsigned int delays[] = { 120, 50, 10, 10, 0 };
	unsigned int i;
	int ret;

	mdelay(10);
	for (i = 0; i < ARRAY_SIZE(order); i++) {
		if (!(ctx->enabled & BIT(order[i])))
			continue;
		ret = dm_gpio_set_value(&ctx->controls[order[i]], 0);
		if (ret)
			return ret;
		ctx->enabled &= ~BIT(order[i]);
		if (delays[i])
			mdelay(delays[i]);
	}
	return 0;
}

static int freetab_prepare(struct freetab_panel *ctx)
{
	static const unsigned int delays[] = { 100, 50, 50, 50, 120, 10 };
	unsigned int i;
	int ret, cleanup;

	for (i = 0; i < ARRAY_SIZE(delays); i++) {
		ret = dm_gpio_set_dir_flags(&ctx->controls[i],
					    GPIOD_IS_OUT | GPIOD_IS_OUT_ACTIVE);
		if (ret) {
			cleanup = freetab_power_down(ctx);
			return cleanup ? cleanup : ret;
		}
		ctx->enabled |= BIT(i);
		mdelay(delays[i]);
	}
	return 0;
}

static int freetab_enable_backlight(struct udevice *dev)
{
	struct freetab_panel *ctx = dev_get_priv(dev);

	return backlight_enable(ctx->backlight);
}

static int freetab_set_backlight(struct udevice *dev, int percent)
{
	struct freetab_panel *ctx = dev_get_priv(dev);

	return backlight_set_brightness(ctx->backlight, percent);
}

static int freetab_get_timing(struct udevice *dev, struct display_timing *timing)
{
	*timing = (struct display_timing) {
		.pixelclock = { 69333000, 69333000, 69333000 },
		.hactive = { 1024, 1024, 1024 },
		.hfront_porch = { 40, 40, 40 },
		.hsync_len = { 128, 128, 128 },
		.hback_porch = { 152, 152, 152 },
		.vactive = { 768, 768, 768 },
		.vfront_porch = { 10, 10, 10 },
		.vsync_len = { 3, 3, 3 },
		.vback_porch = { 12, 12, 12 },
		.flags = DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
			 DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_NEGEDGE,
	};
	return 0;
}

static int freetab_probe(struct udevice *dev)
{
	struct freetab_panel *ctx = dev_get_priv(dev);
	int ret;

	ret = uclass_get_device_by_phandle(UCLASS_PANEL_BACKLIGHT, dev,
					   "backlight", &ctx->backlight);
	if (ret)
		return ret;
	ret = gpio_get_list_count(dev, "modecom,control-gpios");
	if (ret != ARRAY_SIZE(ctx->controls))
		return ret < 0 ? ret : -EINVAL;
	ret = gpio_request_list_by_name(dev, "modecom,control-gpios",
					ctx->controls, ARRAY_SIZE(ctx->controls), 0);
	if (ret < 0)
		return ret;
	ret = freetab_prepare(ctx);
	if (ret)
		gpio_free_list(dev, ctx->controls, ARRAY_SIZE(ctx->controls));
	return ret;
}

static int freetab_remove(struct udevice *dev)
{
	struct freetab_panel *ctx = dev_get_priv(dev);
	int ret;

	ret = freetab_set_backlight(dev, 0);
	if (ret)
		return ret;
	ret = freetab_power_down(ctx);
	if (ret)
		return ret;
	return gpio_free_list(dev, ctx->controls, ARRAY_SIZE(ctx->controls));
}

static const struct panel_ops freetab_ops = {
	.enable_backlight = freetab_enable_backlight,
	.set_backlight = freetab_set_backlight,
	.get_display_timing = freetab_get_timing,
};

static const struct udevice_id freetab_ids[] = {
	{ .compatible = "modecom,freetab8001-display" },
	{ }
};

U_BOOT_DRIVER(freetab_panel) = {
	.name = "freetab_panel",
	.id = UCLASS_PANEL,
	.of_match = freetab_ids,
	.ops = &freetab_ops,
	.probe = freetab_probe,
	.remove = freetab_remove,
	.priv_auto = sizeof(struct freetab_panel),
};
