// SPDX-License-Identifier: GPL-2.0-only
#include <adc.h>
#include <dm.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <power/pmic.h>

#define MT6329_ADC_CONTROL	0xe8
#define MT6329_ADC_CONFIG		0xe9
#define MT6329_ADC_BIAS		0x38
#define MT6329_ADC_START		BIT(0)
#define MT6329_ADC_TDET		BIT(5)
#define MT6329_ADC_SAMPLES	16

struct mt6329_adc_priv {
	int channel;
	int gain;
	int offset;
	unsigned int value;
	int saved_control;
	int saved_config;
	int saved_bias;
	bool restore_pending;
	bool restore_temperature;
	bool busy;
};

static int mt6329_adc_read_once(struct udevice *dev, unsigned int channel,
				unsigned int *value)
{
	struct udevice *pmic = dev->parent;
	unsigned int result = 0xda + 2 * channel;
	int ret, low, high;

	ret = pmic_clrsetbits(pmic, MT6329_ADC_CONTROL, MT6329_ADC_START, 0);
	if (ret)
		return ret;
	ret = pmic_clrsetbits(pmic, MT6329_ADC_CONTROL, 0, MT6329_ADC_START);
	if (ret)
		return ret;
	udelay(30);
	ret = read_poll_timeout(pmic_reg_read, high, high < 0 || (high & BIT(7)),
				100, 100000, pmic, result + 1);
	if (high < 0)
		return high;
	if (ret)
		return ret;
	low = pmic_reg_read(pmic, result);
	if (low < 0)
		return low;
	high = pmic_reg_read(pmic, result + 1);
	if (high < 0)
		return high;
	if (!(high & BIT(7)))
		return -EIO;
	*value = low | (high & 3) << 8;

	return 0;
}

static int mt6329_adc_restore(struct udevice *dev)
{
	struct mt6329_adc_priv *priv = dev_get_priv(dev);
	struct udevice *pmic = dev->parent;
	int ret, err;

	if (!priv->restore_pending)
		return 0;
	ret = pmic_clrsetbits(pmic, MT6329_ADC_CONTROL, MT6329_ADC_START, 0);
	if (priv->restore_temperature) {
		err = pmic_clrsetbits(pmic, MT6329_ADC_BIAS, MT6329_ADC_TDET,
				      priv->saved_bias & MT6329_ADC_TDET);
		if (!ret)
			ret = err;
	}
	err = pmic_reg_write(pmic, MT6329_ADC_CONFIG, priv->saved_config);
	if (!ret)
		ret = err;
	err = pmic_reg_write(pmic, MT6329_ADC_CONTROL, priv->saved_control);
	if (!ret)
		ret = err;
	if (!ret)
		priv->restore_pending = false;

	return ret;
}

static int mt6329_adc_sample(struct udevice *dev, int channel)
{
	struct mt6329_adc_priv *priv = dev_get_priv(dev);
	struct udevice *pmic = dev->parent;
	unsigned int hw_channel = channel ? 3 : 1;
	unsigned int sum = 0, value;
	int control, config, bias = 0, ret, cleanup;

	if (channel < 0 || channel > 1)
		return -EINVAL;
	priv->channel = -1;
	ret = mt6329_adc_restore(dev);
	if (ret)
		return ret;
	control = pmic_reg_read(pmic, MT6329_ADC_CONTROL);
	if (control < 0)
		return control;
	if (control & MT6329_ADC_START)
		return -EBUSY;
	config = pmic_reg_read(pmic, MT6329_ADC_CONFIG);
	if (config < 0)
		return config;
	if (channel) {
		bias = pmic_reg_read(pmic, MT6329_ADC_BIAS);
		if (bias < 0)
			return bias;
	}
	priv->saved_control = control;
	priv->saved_config = config;
	priv->saved_bias = bias;
	priv->restore_temperature = channel == 1;
	priv->restore_pending = true;
	ret = pmic_clrsetbits(pmic, MT6329_ADC_CONTROL, 0xf2, hw_channel << 4 | 2);
	if (ret)
		goto restore;
	ret = pmic_clrsetbits(pmic, MT6329_ADC_CONFIG, 0xf3,
			      channel ? 0xf3 : 0x11 | (config & 2));
	if (ret)
		goto restore;
	if (channel) {
		ret = pmic_clrsetbits(pmic, MT6329_ADC_BIAS, 0, MT6329_ADC_TDET);
		if (ret)
			goto restore;
		mdelay(50);
	}
	for (unsigned int i = 0; i < MT6329_ADC_SAMPLES; i++) {
		ret = mt6329_adc_read_once(dev, hw_channel, &value);
		if (ret)
			goto restore;
		sum += value;
	}

restore:
	cleanup = mt6329_adc_restore(dev);
	if (ret || cleanup)
		return ret ?: cleanup;
	priv->value = (sum + MT6329_ADC_SAMPLES / 2) / MT6329_ADC_SAMPLES;
	priv->channel = channel;

	return 0;
}

static int mt6329_adc_start_channel(struct udevice *dev, int channel)
{
	struct mt6329_adc_priv *priv = dev_get_priv(dev);
	int ret;

	if (priv->busy)
		return -EBUSY;
	priv->busy = true;
	ret = mt6329_adc_sample(dev, channel);
	priv->busy = false;
	return ret;
}

static int mt6329_adc_channel_data(struct udevice *dev, int channel,
				   unsigned int *data)
{
	struct mt6329_adc_priv *priv = dev_get_priv(dev);

	if (priv->busy)
		return -EBUSY;
	if (channel != priv->channel || priv->channel < 0)
		return -ENODATA;
	*data = priv->value;

	return 0;
}

static int mt6329_adc_channel_raw_to_uV(struct udevice *dev, int channel,
					unsigned int raw, int *uV)
{
	struct mt6329_adc_priv *priv = dev_get_priv(dev);
	int corrected;

	if (channel < 0 || channel > 1)
		return -EINVAL;
	if (raw > 1023)
		return -ERANGE;
	corrected = (int)raw + (int)raw * priv->gain / 1024 + priv->offset;
	if (corrected < 0)
		return -ERANGE;
	*uV = ((u64)corrected * (channel ? 1200000 : 4800000)) >> 10;

	return 0;
}

static int mt6329_adc_stop(struct udevice *dev)
{
	struct mt6329_adc_priv *priv = dev_get_priv(dev);
	int ret;

	if (priv->busy)
		return -EBUSY;
	priv->busy = true;
	priv->channel = -1;
	ret = mt6329_adc_restore(dev);
	priv->busy = false;
	return ret;
}

static int mt6329_adc_probe(struct udevice *dev)
{
	struct mt6329_adc_priv *priv = dev_get_priv(dev);
	struct adc_uclass_plat *plat = dev_get_uclass_plat(dev);
	int low, high;

	priv->channel = -1;
	low = pmic_reg_read(dev->parent, 0x192);
	if (low < 0)
		return low;
	high = pmic_reg_read(dev->parent, 0x193);
	if (high < 0)
		return high;
	priv->gain = low & 0x7f;
	if (priv->gain > 64)
		priv->gain -= 128;
	priv->offset = (low >> 7) | ((high & 0x1f) << 1);
	if (priv->offset > 32)
		priv->offset -= 64;
	plat->data_format = ADC_DATA_FORMAT_BIN;
	plat->data_mask = 1023;
	plat->channel_mask = 3;
	plat->vdd_microvolts = -ENODATA;
	plat->vss_microvolts = 0;

	return 0;
}

static const struct adc_ops mt6329_adc_ops = {
	.start_channel = mt6329_adc_start_channel,
	.channel_data = mt6329_adc_channel_data,
	.channel_raw_to_uv = mt6329_adc_channel_raw_to_uV,
	.stop = mt6329_adc_stop,
};

static const struct udevice_id mt6329_adc_ids[] = {
	{ .compatible = "mediatek,mt6329-auxadc" },
	{ }
};

U_BOOT_DRIVER(mt6329_adc) = {
	.name = "mt6329_adc",
	.id = UCLASS_ADC,
	.of_match = mt6329_adc_ids,
	.probe = mt6329_adc_probe,
	.remove = mt6329_adc_stop,
	.ops = &mt6329_adc_ops,
	.priv_auto = sizeof(struct mt6329_adc_priv),
	.flags = DM_FLAG_OS_PREPARE,
};
