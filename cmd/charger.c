// SPDX-License-Identifier: GPL-2.0-only
#include <command.h>
#include <dm.h>
#include <power/charger.h>

static int do_charger(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	static const char *const states[] = {
		[CHARGER_STATE_UNKNOWN] = "unknown",
		[CHARGER_STATE_DISCHARGING] = "discharging",
		[CHARGER_STATE_NOT_CHARGING] = "not-charging",
		[CHARGER_STATE_CHARGING] = "charging",
		[CHARGER_STATE_FULL] = "full",
	};
	static const char *const health[] = {
		[CHARGER_HEALTH_UNKNOWN] = "unknown",
		[CHARGER_HEALTH_GOOD] = "good",
		[CHARGER_HEALTH_COLD] = "cold",
		[CHARGER_HEALTH_OVERHEAT] = "overheat",
		[CHARGER_HEALTH_OVERVOLTAGE] = "overvoltage",
		[CHARGER_HEALTH_WATCHDOG] = "watchdog",
		[CHARGER_HEALTH_SAFETY_TIMER] = "safety-timer",
		[CHARGER_HEALTH_FAULT] = "fault",
	};
	struct charger_status status;
	struct udevice *dev;
	int ret;

	if (argc != 3 || strcmp(argv[1], "status"))
		return CMD_RET_USAGE;
	ret = uclass_get_device_by_name(UCLASS_CHARGER, argv[2], &dev);
	if (ret) {
		printf("Charger %s unavailable: %d\n", argv[2], ret);
		return CMD_RET_FAILURE;
	}
	ret = charger_get_status(dev, &status);
	if (ret) {
		printf("Charger %s status failed: %d\n", dev->name, ret);
		return CMD_RET_FAILURE;
	}
	printf("%s: %s, health: %s, online: %s\n", dev->name,
	       (unsigned int)status.state < ARRAY_SIZE(states) ?
	       states[status.state] : "unknown",
	       (unsigned int)status.health < ARRAY_SIZE(health) ?
	       health[status.health] : "unknown", status.online ? "yes" : "no");
	printf("voltage: %d uV, temperature: %d mC, input-limit: ",
	       status.voltage_uv, status.temp_mc);
	if (status.input_current_valid)
		printf("%d uA\n", status.input_current_ua);
	else
		printf("unknown\n");
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(charger, 3, 1, do_charger,
	   "display charger status",
	   "status <device>"
);
