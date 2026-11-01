// SPDX-License-Identifier: GPL-2.0-only
#include <dm.h>
#include <power/charger.h>

int charger_get_status(struct udevice *dev, struct charger_status *status)
{
	const struct charger_ops *ops = device_get_ops(dev);
	int ret;

	if (!status)
		return -EINVAL;
	memset(status, 0, sizeof(*status));
	if (!ops || !ops->get_status)
		return -ENOSYS;
	ret = ops->get_status(dev, status);
	if (ret)
		memset(status, 0, sizeof(*status));
	return ret;
}

int charger_set_input_current(struct udevice *dev, int ua)
{
	const struct charger_ops *ops = device_get_ops(dev);

	if (ua < 0)
		return -EINVAL;
	if (!ops || !ops->set_input_current)
		return -ENOSYS;
	return ops->set_input_current(dev, ua);
}

UCLASS_DRIVER(charger) = {
	.id = UCLASS_CHARGER,
	.name = "charger",
};
