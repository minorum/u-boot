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

enum charger_usb_type {
	CHARGER_USB_UNKNOWN,
	CHARGER_USB_SDP,
	CHARGER_USB_DCP,
	CHARGER_USB_CDP,
};

struct charger_usb_source {
	bool online;
	enum charger_usb_type type;
	int current_max_ua;
};

struct charger_status {
	enum charger_state state;
	enum charger_health health;
	bool online;
	bool input_present;
	bool input_current_valid;
	int voltage_uv;
	int temp_mc;
	int input_current_ua;
	int input_budget_ua;
	bool usb_source_valid;
	struct charger_usb_source usb_source;
};

struct charger_ops {
	int (*get_status)(struct udevice *dev, struct charger_status *status);
	int (*set_input_current)(struct udevice *dev, int ua);
	int (*get_usb_source)(struct udevice *dev, struct charger_usb_source *source);
	int (*set_usb_current)(struct udevice *dev, int ua);
	int (*set_usb_event_poll)(struct udevice *dev, int (*poll)(void *), void *data);
};

int charger_get_status(struct udevice *dev, struct charger_status *status);
int charger_set_input_current(struct udevice *dev, int ua);
int charger_get_usb_source(struct udevice *dev, struct charger_usb_source *source);
int charger_set_usb_current(struct udevice *dev, int ua);
int charger_set_usb_event_poll(struct udevice *dev, int (*poll)(void *), void *data);

#endif
