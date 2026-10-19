// SPDX-License-Identifier: GPL-2.0-or-later
#include <dm.h>
#include <env.h>
#include <init.h>
#include <asm/io.h>
#include <reboot-mode/reboot-mode.h>

struct j320fn_reboot_mode {
	void __iomem *mailbox;
};

static int j320fn_reboot_mode_get(struct udevice *dev, u32 *value)
{
	struct j320fn_reboot_mode *mode = dev_get_priv(dev);

	*value = readl(mode->mailbox);
	return 0;
}

static int j320fn_reboot_mode_set(struct udevice *dev, u32 value)
{
	struct j320fn_reboot_mode *mode = dev_get_priv(dev);

	writel(value, mode->mailbox);
	return readl(mode->mailbox) == value ? 0 : -EIO;
}

static int j320fn_reboot_mode_probe(struct udevice *dev)
{
	struct j320fn_reboot_mode *mode = dev_get_priv(dev);
	fdt_size_t size;

	if (dev_read_addr_size(dev, &size) == FDT_ADDR_T_NONE ||
	    size != sizeof(u32))
		return -EINVAL;

	mode->mailbox = dev_remap_addr(dev);
	return mode->mailbox ? 0 : -EINVAL;
}

static const struct reboot_mode_ops j320fn_reboot_mode_ops = {
	.get = j320fn_reboot_mode_get,
	.set = j320fn_reboot_mode_set,
};

static const struct udevice_id j320fn_reboot_mode_ids[] = {
	{ .compatible = "sprd,sc9830-reboot-mode" },
	{ }
};

U_BOOT_DRIVER(j320fn_reboot_mode) = {
	.name = "j320fn-reboot-mode",
	.id = UCLASS_REBOOT_MODE,
	.of_match = j320fn_reboot_mode_ids,
	.probe = j320fn_reboot_mode_probe,
	.priv_auto = sizeof(struct j320fn_reboot_mode),
	.ops = &j320fn_reboot_mode_ops,
};

int board_late_init(void)
{
	struct udevice *dev;
	int ret;

	ret = uclass_get_device(UCLASS_REBOOT_MODE, 0, &dev);
	if (!ret)
		ret = dm_reboot_mode_update(dev);
	if (ret) {
		env_set("reboot-mode", NULL);
		printf("Reboot mailbox failed: %d\n", ret);
	}
	return 0;
}
