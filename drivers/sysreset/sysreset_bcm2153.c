// SPDX-License-Identifier: GPL-2.0+

#include <dm.h>
#include <sysreset.h>
#include <asm/io.h>
#include <dm/read.h>
#include <linux/errno.h>

static int bcm2153_sysreset_request(struct udevice *dev, enum sysreset_t type)
{
	void *reg;

	if (type != SYSRESET_WARM && type != SYSRESET_COLD)
		return -EPROTONOSUPPORT;

	reg = dev_read_addr_ptr(dev);
	if (!reg)
		return -EINVAL;

	setbits_le16(reg, BIT(3));
	return -EINPROGRESS;
}

static const struct sysreset_ops bcm2153_sysreset_ops = {
	.request = bcm2153_sysreset_request,
};

static const struct udevice_id bcm2153_sysreset_ids[] = {
	{ .compatible = "brcm,bcm2153-reset" },
	{ }
};

U_BOOT_DRIVER(bcm2153_sysreset) = {
	.name = "bcm2153_sysreset",
	.id = UCLASS_SYSRESET,
	.of_match = bcm2153_sysreset_ids,
	.ops = &bcm2153_sysreset_ops,
};
