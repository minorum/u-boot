// SPDX-License-Identifier: GPL-2.0+
#include <command.h>
#include <dm.h>
#include <dm/uclass.h>
#include <reboot-mode/reboot-mode.h>

static int do_reboot_mode(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	const struct reboot_mode_uclass_platdata *plat;
	struct reboot_mode_ops *ops;
	struct udevice *dev;
	int i, ret;

	if (argc != 3)
		return CMD_RET_USAGE;
	ret = uclass_get_device_by_name(UCLASS_REBOOT_MODE, argv[1], &dev);
	if (ret)
		return CMD_RET_FAILURE;
	ops = reboot_mode_get_ops(dev);
	if (!ops->set)
		return CMD_RET_FAILURE;
	plat = dev_get_uclass_plat(dev);
	for (i = 0; i < plat->count; i++) {
		if (!strcmp(plat->modes[i].mode_name, argv[2]))
			return ops->set(dev, plat->modes[i].mode_id) ?
				CMD_RET_FAILURE : CMD_RET_SUCCESS;
	}
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(reboot_mode, 3, 0, do_reboot_mode,
	   "write a named reboot mode", "<device> <mode>");
