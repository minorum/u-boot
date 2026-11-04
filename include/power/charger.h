/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __POWER_CHARGER_H
#define __POWER_CHARGER_H

#include <linux/types.h>

struct udevice;

enum charger_state {
	CHARGER_STATE_UNKNOWN,
	CHARGER_STATE_DISCHARGING,
	CHARGER_STATE_NOT_CHARGING,
	CHARGER_STATE_CHARGING,
	CHARGER_STATE_FULL,
};

enum charger_health {
	CHARGER_HEALTH_UNKNOWN,
	CHARGER_HEALTH_GOOD,
	CHARGER_HEALTH_COLD,
	CHARGER_HEALTH_OVERHEAT,
	CHARGER_HEALTH_OVERVOLTAGE,
	CHARGER_HEALTH_WATCHDOG,
	CHARGER_HEALTH_SAFETY_TIMER,
	CHARGER_HEALTH_FAULT,
};

struct charger_status {
	enum charger_state state;
	enum charger_health health;
	bool online;
	bool input_present;
	bool input_current_valid;
	bool inhibit_charge;
	int voltage_uv;
	int temp_mc;
	int input_current_ua;
	int input_budget_ua;
};

struct charger_ops {
	int (*get_status)(struct udevice *dev, struct charger_status *status);
	int (*set_input_current)(struct udevice *dev, int ua);
	int (*set_inhibit)(struct udevice *dev, bool inhibit);
};

int charger_get_status(struct udevice *dev, struct charger_status *status);
int charger_set_input_current(struct udevice *dev, int ua);
int charger_set_inhibit(struct udevice *dev, bool inhibit);

#endif
