// SPDX-License-Identifier: GPL-2.0-only
#include <adc.h>
#include <cyclic.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <i2c.h>
#include <limits.h>
#include <thermal.h>
#include <power/charger.h>
#include <linux/kernel.h>

#define NCP1851_STATUS 0x00
#define NCP1851_CTRL1 0x01
#define NCP1851_CTRL2 0x02
#define NCP1851_CH2_INT 0x05
#define NCP1851_VIN 0x07
#define NCP1851_VBAT 0x08
#define NCP1851_TEMP 0x09
#define NCP1851_VREG 0x0e
#define NCP1851_IBAT 0x0f
#define NCP1851_MISC 0x10
#define NCP1851_MAX_REG 0x12
#define NCP1851_CHGEN BIT(6)
#define NCP1851_RESTART BIT(1)
#define NCP1851_TIMER_FAULTS 0x0e
#define NCP1851_INPUT_READY 0x0c
#define NCP1851_POLL_MS 10000

static const int ncp1851_input_limits[] = { 100000, 500000, 900000, 1500000 };

struct ncp1851_history {
	unsigned int timer_fault;
	bool fault_latched, cycle_pending;
};

struct ncp1851 {
	struct ncp1851_history *history;
	struct udevice *dev, *adc, *thermal;
	struct cyclic_info cyclic;
	unsigned int channel;
	u8 regs[NCP1851_MAX_REG + 1];
	unsigned int vreg, ichg, ieoc, iweak, vsys;
	int temp_mc, voltage_uv, temp_min_mc, temp_max_mc, overvoltage_uv;
	int input_max, input_budget, health, status;
	bool power_path, online, busy, stopped;
};

static int ncp1851_read(struct ncp1851 *chip, unsigned int reg,
			unsigned int *value)
{
	int ret = dm_i2c_reg_read(chip->dev, reg);

	if (ret < 0)
		return ret;
	*value = ret;
	return 0;
}

static int ncp1851_update_bits(struct ncp1851 *chip, unsigned int reg,
			       unsigned int mask, unsigned int value)
{
	unsigned int old;
	int ret;

	ret = ncp1851_read(chip, reg, &old);
	if (ret)
		return ret;
	value = (old & ~mask) | (value & mask);
	if (value == old)
		return 0;
	return dm_i2c_reg_write(chip->dev, reg, value);
}

static int ncp1851_encode(unsigned int value, unsigned int min,
			  unsigned int step, unsigned int max_code,
			  unsigned int *code)
{
	if (value < min || value > min + max_code * step)
		return -EINVAL;
	*code = (value - min) / step;
	return 0;
}

static int ncp1851_input_code(int ua)
{
	int i;

	for (i = ARRAY_SIZE(ncp1851_input_limits) - 1; i >= 0; i--)
		if (ua >= ncp1851_input_limits[i])
			return i;
	return -EINVAL;
}

static int ncp1851_ctrl1(struct ncp1851 *chip, bool enabled, bool restart)
{
	unsigned int old, value;
	int ret;

	ret = ncp1851_read(chip, NCP1851_CTRL1, &old);
	if (ret)
		return ret;
	value = (old & 0x0c) | BIT(0);
	if (enabled)
		value |= NCP1851_CHGEN;
	if (restart)
		value |= NCP1851_RESTART;
	if (value == old && !restart)
		return 0;
	return dm_i2c_reg_write(chip->dev, NCP1851_CTRL1, value);
}

static int ncp1851_kick(struct ncp1851 *chip)
{
	unsigned int value;
	int ret;

	ret = ncp1851_read(chip, NCP1851_CTRL2, &value);
	if (ret)
		return ret;
	return dm_i2c_reg_write(chip->dev, NCP1851_CTRL2, value & ~0xc0);
}

static int ncp1851_inhibit(struct ncp1851 *chip)
{
	unsigned int value;
	int ret;

	ret = ncp1851_ctrl1(chip, false, false);
	if (ret)
		return ret;
	ret = ncp1851_read(chip, NCP1851_CTRL1, &value);
	if (ret)
		return ret;
	if (value & NCP1851_CHGEN)
		return -EIO;
	return ncp1851_update_bits(chip, NCP1851_CTRL2, BIT(7), BIT(7));
}

static int ncp1851_read_regs(struct ncp1851 *chip)
{
	static const u8 addresses[] = { 0, 1, 2, 5, 7, 8, 9, 14, 15, 16 };
	u8 regs[NCP1851_MAX_REG + 1] = {};
	unsigned int value;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(addresses); i++) {
		ret = ncp1851_read(chip, addresses[i], &value);
		if (ret)
			return ret;
		regs[addresses[i]] = value;
		if (addresses[i] == NCP1851_CH2_INT)
			chip->history->timer_fault |= value & NCP1851_TIMER_FAULTS;
	}
	memcpy(chip->regs, regs, sizeof(regs));
	if ((regs[NCP1851_STATUS] >> 4) == 11 &&
	    (regs[NCP1851_CTRL1] & NCP1851_CHGEN))
		chip->history->fault_latched = true;
	return 0;
}

static int ncp1851_status(unsigned int state, bool online, bool enabled)
{
	if (!online)
		return CHARGER_STATE_DISCHARGING;
	if (!enabled)
		return CHARGER_STATE_NOT_CHARGING;
	switch (state) {
	case 2:
	case 3:
	case 4:
	case 5:
	case 9:
	case 10:
		return CHARGER_STATE_CHARGING;
	case 6:
		return CHARGER_STATE_FULL;
	default:
		return CHARGER_STATE_NOT_CHARGING;
	}
}

static int ncp1851_health(struct ncp1851 *chip)
{
	if (chip->temp_mc <= chip->temp_min_mc)
		return CHARGER_HEALTH_COLD;
	if (chip->temp_mc >= chip->temp_max_mc ||
	    (chip->regs[NCP1851_TEMP] & BIT(3)))
		return CHARGER_HEALTH_OVERHEAT;
	if ((chip->regs[NCP1851_VIN] & 0xa0) ||
	    (chip->regs[NCP1851_VBAT] & BIT(6)) ||
	    chip->voltage_uv >= chip->overvoltage_uv)
		return CHARGER_HEALTH_OVERVOLTAGE;
	if (chip->history->timer_fault & BIT(3))
		return CHARGER_HEALTH_WATCHDOG;
	if (chip->history->timer_fault & BIT(1))
		return CHARGER_HEALTH_SAFETY_TIMER;
	if (chip->history->fault_latched || (chip->history->timer_fault & BIT(2)) ||
	    ((chip->regs[NCP1851_STATUS] >> 4) == 11 &&
	     (chip->regs[NCP1851_CTRL1] & NCP1851_CHGEN)))
		return CHARGER_HEALTH_FAULT;
	return CHARGER_HEALTH_GOOD;
}

static int ncp1851_configure(struct ncp1851 *chip, int input)
{
	int ret;

	ret = ncp1851_ctrl1(chip, false, false);
	if (ret)
		return ret;
	ret = ncp1851_update_bits(chip, NCP1851_MISC, 0x7f,
				  (chip->iweak << 5) | (chip->vsys << 2) |
					  input);
	if (ret)
		return ret;
	ret = ncp1851_update_bits(chip, NCP1851_VREG, 0x3f, chip->vreg);
	if (ret)
		return ret;
	ret = ncp1851_update_bits(chip, NCP1851_IBAT, 0x7f,
				  (chip->ieoc << 4) | chip->ichg);
	if (ret)
		return ret;
	return ncp1851_update_bits(chip, NCP1851_CTRL2, 0xef,
				   (chip->power_path ? BIT(5) : 0) | BIT(1));
}

static bool ncp1851_configured(struct ncp1851 *chip, int input)
{
	return (chip->regs[NCP1851_VREG] & 0x3f) == chip->vreg &&
	       (chip->regs[NCP1851_IBAT] & 0x7f) ==
		       ((chip->ieoc << 4) | chip->ichg) &&
	       (chip->regs[NCP1851_MISC] & 0x7f) ==
		       ((chip->iweak << 5) | (chip->vsys << 2) | input) &&
	       (chip->regs[NCP1851_CTRL2] & 0xef) ==
		       ((chip->power_path ? BIT(5) : 0) | BIT(1)) &&
	       !(chip->regs[NCP1851_CTRL1] & 0xb0);
}

static int ncp1851_verify_config(struct ncp1851 *chip, int input, bool enabled)
{
	int ret;

	ret = ncp1851_read_regs(chip);
	if (ret)
		return ret;
	if (!ncp1851_configured(chip, input) ||
	    !!(chip->regs[NCP1851_CTRL1] & NCP1851_CHGEN) != enabled)
		return -EIO;
	return 0;
}

static int ncp1851_measure(struct ncp1851 *chip)
{
	unsigned int raw;
	int ret, cleanup;

	ret = adc_start_channel(chip->adc, chip->channel);
	if (ret)
		return ret;
	ret = adc_channel_data(chip->adc, chip->channel, &raw);
	if (!ret)
		ret = adc_channel_raw_to_uV(chip->adc, chip->channel, raw,
					    &chip->voltage_uv);
	cleanup = adc_stop(chip->adc);
	if (ret)
		return ret;
	if (cleanup && cleanup != -ENOSYS)
		return cleanup;
	if (chip->voltage_uv <= 0)
		return -ERANGE;
	return thermal_get_temp(chip->thermal, &chip->temp_mc);
}

static int ncp1851_update(struct ncp1851 *chip)
{
	int ret, input, health;
	bool online, restart;

	ret = ncp1851_read_regs(chip);
	if (ret)
		goto fail;
	online = (chip->regs[NCP1851_VIN] & NCP1851_INPUT_READY) ==
			 NCP1851_INPUT_READY &&
		 !(chip->regs[NCP1851_CTRL1] & BIT(5));
	if (!(chip->regs[NCP1851_VIN] & BIT(3))) {
		chip->history->timer_fault = 0;
		chip->history->fault_latched = false;
		chip->history->cycle_pending = true;
		chip->input_budget = min(chip->input_budget, 100000);
	}
	chip->online = online;
	input = ncp1851_input_code(min(chip->input_budget, chip->input_max));
	if (!online || input < 0 || chip->health != CHARGER_HEALTH_GOOD ||
	    ncp1851_health(chip) != CHARGER_HEALTH_GOOD ||
	    !ncp1851_configured(chip, input)) {
		ret = ncp1851_inhibit(chip);
		if (ret)
			goto fail;
	}
	ret = ncp1851_measure(chip);
	if (ret)
		goto fail;
	health = ncp1851_health(chip);
	chip->health = health;
	if (!online || input < 0 || health != CHARGER_HEALTH_GOOD) {
		ret = ncp1851_inhibit(chip);
		if (ret)
			goto fail;
		chip->status = ncp1851_status(0, online, false);
		return 0;
	}
	if (!ncp1851_configured(chip, input)) {
		ret = ncp1851_configure(chip, input);
		if (ret)
			goto fail;
		ret = ncp1851_verify_config(chip, input, false);
		if (ret)
			goto fail;
		if (ncp1851_health(chip) != CHARGER_HEALTH_GOOD)
			goto check_health;
	}
	restart = chip->history->cycle_pending;
	chip->history->cycle_pending = false;
	ret = ncp1851_ctrl1(chip, true, restart);
	if (ret)
		goto fail;
	ret = ncp1851_kick(chip);
	if (ret)
		goto fail;
	ret = ncp1851_verify_config(chip, input, true);
	if (ret)
		goto fail;
check_health:
	chip->health = ncp1851_health(chip);
	if (chip->health != CHARGER_HEALTH_GOOD) {
		ret = ncp1851_inhibit(chip);
		if (ret)
			goto fail;
	}
	chip->status = ncp1851_status(chip->regs[NCP1851_STATUS] >> 4, online,
				      chip->health == CHARGER_HEALTH_GOOD);
	return 0;
fail:
	chip->status = CHARGER_STATE_UNKNOWN;
	chip->health = CHARGER_HEALTH_UNKNOWN;
	if (ncp1851_inhibit(chip))
		dev_err(chip->dev, "Cannot inhibit charging\n");
	return ret;
}

static int ncp1851_refresh(struct ncp1851 *chip)
{
	int ret;

	if (chip->busy)
		return -EBUSY;
	if (chip->stopped)
		return -ESHUTDOWN;
	chip->busy = true;
	ret = ncp1851_update(chip);
	chip->busy = false;
	return ret;
}

static void ncp1851_periodic(struct cyclic_info *cyclic)
{
	struct ncp1851 *chip = container_of(cyclic, struct ncp1851, cyclic);

	if (!chip->busy && !chip->stopped)
		ncp1851_refresh(chip);
}

static int ncp1851_get_status(struct udevice *dev,
			      struct charger_status *status)
{
	struct ncp1851 *chip = dev_get_priv(dev);
	int ret;

	ret = ncp1851_refresh(chip);
	if (ret)
		return ret;
	status->state = chip->status;
	status->health = chip->health;
	status->online = chip->online;
	status->voltage_uv = chip->voltage_uv;
	status->temp_mc = chip->temp_mc;
	status->input_current_valid = (chip->regs[NCP1851_CTRL2] & 6) == 2;
	status->input_current_ua = status->input_current_valid ?
		ncp1851_input_limits[chip->regs[NCP1851_MISC] & 3] : 0;
	return 0;
}

static int ncp1851_set_input_current(struct udevice *dev, int ua)
{
	struct ncp1851 *chip = dev_get_priv(dev);

	if (ua < 0)
		return -EINVAL;
	if (chip->busy)
		return -EBUSY;
	if (chip->stopped)
		return -ESHUTDOWN;
	chip->input_budget = min(ua, chip->input_max);
	return ncp1851_refresh(chip);
}

static int ncp1851_remove(struct udevice *dev)
{
	struct ncp1851 *chip = dev_get_priv(dev);
	int ret;

	if (chip->busy)
		return -EBUSY;
	cyclic_unregister(&chip->cyclic);
	chip->stopped = true;
	chip->busy = true;
	ret = ncp1851_inhibit(chip);
	chip->busy = false;
	return ret;
}

static int ncp1851_read_profile(struct ncp1851 *chip)
{
	ofnode battery = ofnode_parse_phandle(dev_ofnode(chip->dev),
					      "monitored-battery", 0);
	u32 cc, cv, precharge, termination, overvoltage, limits[2], value;
	int ret;

	if (!ofnode_valid(battery) ||
	    !ofnode_device_is_compatible(battery, "simple-battery"))
		return -EINVAL;
	if (ofnode_read_u32(battery, "constant-charge-current-max-microamp",
			    &cc) ||
	    ofnode_read_u32(battery, "constant-charge-voltage-max-microvolt",
			    &cv) ||
	    ofnode_read_u32(battery, "precharge-current-microamp",
			    &precharge) ||
	    ofnode_read_u32(battery, "charge-term-current-microamp",
			    &termination) ||
	    ofnode_read_u32(battery, "over-voltage-threshold-microvolt",
			    &overvoltage) ||
	    ofnode_read_u32_array(battery, "alert-celsius", limits, 2))
		return -EINVAL;
	if (precharge != 100000 || overvoltage <= cv || overvoltage > INT_MAX ||
	    (s32)limits[0] <= INT_MIN / 1000 ||
	    (s32)limits[1] >= INT_MAX / 1000 ||
	    (s32)limits[0] >= (s32)limits[1])
		return -EINVAL;
	ret = ncp1851_encode(cv, 3300000, 25000, 48, &chip->vreg);
	if (ret)
		return ret;
	ret = ncp1851_encode(cc, 400000, 100000, 12, &chip->ichg);
	if (ret)
		return ret;
	ret = ncp1851_encode(termination, 100000, 25000, 7, &chip->ieoc);
	if (ret)
		return ret;
	chip->iweak = 1;
	chip->temp_min_mc = (s32)limits[0] * 1000;
	chip->temp_max_mc = (s32)limits[1] * 1000;
	chip->overvoltage_uv = overvoltage;
	if (dev_read_u32(chip->dev, "shunt-resistor-micro-ohms", &value) ||
	    value != 68000)
		return -EINVAL;
	if (dev_read_u32(chip->dev, "input-current-limit-microamp", &value) ||
	    value > 1500000 || ncp1851_input_code(value) < 0 ||
	    ncp1851_input_limits[ncp1851_input_code(value)] != (int)value)
		return -EINVAL;
	chip->input_max = value;
	chip->power_path = dev_read_bool(chip->dev, "onnn,power-path");
	chip->vsys = 0;
	if (chip->power_path) {
		ret = dev_read_u32(chip->dev, "onnn,system-voltage-microvolt",
				   &value);
		if (ret)
			return ret;
		ret = ncp1851_encode(value, 3100000, 100000, 5, &chip->vsys);
		if (ret)
			return ret;
		if (3100000 + chip->vsys * 100000 >=
		    3300000 + chip->vreg * 25000)
			return -EINVAL;
	}
	return 0;
}

static int ncp1851_probe(struct udevice *dev)
{
	struct ncp1851 *chip = dev_get_priv(dev);
	struct ofnode_phandle_args args;
	int ret;

	chip->dev = dev;
	chip->history = dev_get_plat(dev);
	ret = ncp1851_read_profile(chip);
	if (ret)
		return ret;
	ret = dev_read_phandle_with_args(dev, "io-channels",
					 "#io-channel-cells", 0, 0, &args);
	if (ret)
		return ret;
	if (args.args_count != 1 || args.args[0] > ADC_MAX_CHANNEL)
		return -EINVAL;
	chip->channel = args.args[0];
	ret = uclass_get_device_by_ofnode(UCLASS_ADC, args.node, &chip->adc);
	if (ret)
		return ret;
	ret = dev_read_phandle_with_args(dev, "io-channels",
					 "#io-channel-cells", 0, 1, &args);
	if (ret)
		return ret;
	if (args.args_count)
		return -EINVAL;
	ret = uclass_get_device_by_ofnode(UCLASS_THERMAL, args.node,
					  &chip->thermal);
	if (ret)
		return ret;
	chip->input_budget = 100000;
	chip->stopped = false;
	ret = ncp1851_refresh(chip);
	if (ret)
		return ret;
	cyclic_register(&chip->cyclic, ncp1851_periodic, NCP1851_POLL_MS * 1000,
			dev->name);
	return 0;
}

static int ncp1851_bind(struct udevice *dev)
{
	dev_or_flags(dev, DM_FLAG_PROBE_AFTER_BIND);
	return 0;
}

static const struct charger_ops ncp1851_ops = {
	.get_status = ncp1851_get_status,
	.set_input_current = ncp1851_set_input_current,
};

static const struct udevice_id ncp1851_ids[] = {
	{ .compatible = "onnn,ncp1851" },
	{ .compatible = "onnn,ncp1851a" },
	{}
};

U_BOOT_DRIVER(ncp1851_charger) = {
	.name = "ncp1851-charger",
	.id = UCLASS_CHARGER,
	.of_match = ncp1851_ids,
	.bind = ncp1851_bind,
	.probe = ncp1851_probe,
	.remove = ncp1851_remove,
	.ops = &ncp1851_ops,
	.priv_auto = sizeof(struct ncp1851),
	.plat_auto = sizeof(struct ncp1851_history), /* Persistent fault lockout */
	.flags = DM_FLAG_OS_PREPARE,
};
