// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2016 NVIDIA CORPORATION */
#include <adc.h>
#include <dm.h>
#include <dm/devres.h>
#include <thermal.h>
#include <linux/math64.h>

struct gadc_thermal_info {
	struct udevice *adc;
	unsigned int channel;
	s32 *lookup_table;
	int nlookup_table;
};

static int gadc_thermal_adc_to_temp(struct gadc_thermal_info *gti, int val)
{
	int temp, temp_hi, temp_lo, adc_hi, adc_lo;
	int i;

	if (!gti->lookup_table)
		return val;

	for (i = 0; i < gti->nlookup_table; i++) {
		if (val >= gti->lookup_table[2 * i + 1])
			break;
	}
	if (i == 0) {
		temp = gti->lookup_table[0];
	} else if (i >= gti->nlookup_table) {
		temp = gti->lookup_table[2 * (gti->nlookup_table - 1)];
	} else {
		adc_hi = gti->lookup_table[2 * i - 1];
		adc_lo = gti->lookup_table[2 * i + 1];
		temp_hi = gti->lookup_table[2 * i - 2];
		temp_lo = gti->lookup_table[2 * i];
		temp = temp_hi + div_s64(((s64)temp_lo - temp_hi) * (val - adc_hi),
					adc_lo - adc_hi);
	}

	return temp;
}

static int gadc_thermal_get_temp(struct udevice *dev, int *temp)
{
	struct gadc_thermal_info *gti = dev_get_priv(dev);
	unsigned int raw;
	int ret, cleanup, voltage;

	ret = adc_start_channel(gti->adc, gti->channel);
	if (ret)
		return ret;
	ret = adc_channel_data(gti->adc, gti->channel, &raw);
	if (!ret)
		ret = adc_channel_raw_to_uV(gti->adc, gti->channel, raw, &voltage);
	cleanup = adc_stop(gti->adc);
	if (ret)
		return ret;
	if (cleanup && cleanup != -ENOSYS)
		return cleanup;
	*temp = gadc_thermal_adc_to_temp(gti, voltage / 1000);

	return 0;
}

static int gadc_thermal_probe(struct udevice *dev)
{
	struct gadc_thermal_info *gti = dev_get_priv(dev);
	struct ofnode_phandle_args args;
	int size, ret;

	size = dev_read_size(dev, "temperature-lookup-table");
	if (size < 0)
		return -EOPNOTSUPP;
	if (!size || size % (2 * sizeof(s32)))
		return -EINVAL;
	gti->lookup_table = devm_kmalloc(dev, size, GFP_KERNEL);
	if (!gti->lookup_table)
		return -ENOMEM;
	gti->nlookup_table = size / (2 * sizeof(s32));
	ret = dev_read_u32_array(dev, "temperature-lookup-table",
				 (u32 *)gti->lookup_table, size / sizeof(s32));
	if (ret)
		return ret;
	for (int i = 0; i < gti->nlookup_table; i++) {
		if (gti->lookup_table[2 * i + 1] < 0 ||
		    (i && gti->lookup_table[2 * i - 1] <= gti->lookup_table[2 * i + 1]))
			return -EINVAL;
	}
	ret = dev_read_phandle_with_args(dev, "io-channels", "#io-channel-cells",
					 0, 0, &args);
	if (ret)
		return ret;
	if (args.args_count != 1 || args.args[0] > ADC_MAX_CHANNEL)
		return -EINVAL;
	gti->channel = args.args[0];

	return uclass_get_device_by_ofnode(UCLASS_ADC, args.node, &gti->adc);
}

static const struct dm_thermal_ops gadc_thermal_ops = {
	.get_temp = gadc_thermal_get_temp,
};

static const struct udevice_id gadc_thermal_ids[] = {
	{ .compatible = "generic-adc-thermal" },
	{ }
};

U_BOOT_DRIVER(generic_adc_thermal) = {
	.name = "generic_adc_thermal",
	.id = UCLASS_THERMAL,
	.of_match = gadc_thermal_ids,
	.probe = gadc_thermal_probe,
	.ops = &gadc_thermal_ops,
	.priv_auto = sizeof(struct gadc_thermal_info),
};
