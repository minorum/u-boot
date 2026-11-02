// SPDX-License-Identifier: GPL-2.0-only
#include <dm.h>
#include <clk.h>
#include <fs_loader.h>
#include <malloc.h>
#include <mmc.h>
#include <pwrseq.h>
#include <serial.h>
#include <time.h>
#include <linux/crc16.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <asm/unaligned.h>
#include <asm/gpio.h>
#include <asm/global_data.h>
#include <dm/pinctrl.h>
#include <dm/device_compat.h>
#include <dm/device-internal.h>
#include <dm/read.h>
#include <dm/uclass.h>

DECLARE_GLOBAL_DATA_PTR;

#define MT6628_STP_MAX 2048

struct mt6628_stp {
	u8 data[MT6628_STP_MAX + 6];
	size_t used;
	size_t length;
	u8 tx_seq;
	u8 rx_seq;
	bool full;
	bool pending;
};

static int mt6628_stp_tx(struct mt6628_stp *stp, const u8 *data,
			 size_t length, u8 *wire, size_t capacity)
{
	u16 crc;

	if (!length || length > MT6628_STP_MAX || capacity < length + 6)
		return -EMSGSIZE;
	if (stp->pending)
		return -EBUSY;

	wire[0] = 0x80;
	wire[1] = 0x40 | (length >> 8);
	wire[2] = length;
	wire[3] = 0;
	crc = 0;
	if (stp->full) {
		wire[0] |= stp->tx_seq << 3 | ((stp->rx_seq + 7) & 7);
		wire[3] = wire[0] + wire[1] + wire[2];
		crc = crc16(0, data, length);
		stp->pending = true;
	}
	memcpy(wire + 4, data, length);
	put_unaligned_le16(crc, wire + length + 4);
	return length + 6;
}

static void mt6628_stp_ack(struct mt6628_stp *stp, u8 *wire)
{
	wire[0] = 0x80 | ((stp->rx_seq + 7) & 7);
	wire[1] = 0;
	wire[2] = 0;
	wire[3] = wire[0];
}

static int mt6628_stp_rx(struct mt6628_stp *stp, u8 byte)
{
	u8 ack, seq;
	size_t size;

	if (stp->used >= sizeof(stp->data))
		return -EMSGSIZE;
	stp->data[stp->used++] = byte;
	if (stp->used == 1 &&
	    (stp->full ? (byte & 0xc0) != 0x80 : byte != 0x80))
		return -EPROTO;
	if (stp->used < 4)
		return 0;
	if (stp->used == 4) {
		stp->length = (stp->data[1] & 0xf) << 8 | stp->data[2];
		if (stp->length > MT6628_STP_MAX)
			return -EMSGSIZE;
		if (stp->length ? (stp->data[1] & 0xf0) != 0x40 :
		    (!stp->full || stp->data[1] || (stp->data[0] & 0x38)))
			return -EPROTO;
		if (stp->data[3] != (stp->full ?
		    (u8)(stp->data[0] + stp->data[1] + stp->data[2]) : 0))
			return -EBADMSG;
	}
	size = stp->length ? stp->length + 6 : 4;
	if (stp->used < size)
		return 0;
	stp->used = 0;
	if (stp->length && get_unaligned_le16(stp->data + size - 2) !=
	    (stp->full ? crc16(0, stp->data + 4, stp->length) : 0))
		return -EBADMSG;
	if (!stp->full)
		return 1;
	ack = stp->data[0] & 7;
	if (stp->pending && ack == stp->tx_seq) {
		stp->pending = false;
		stp->tx_seq = (stp->tx_seq + 1) & 7;
	}
	if (!stp->length)
		return 2;
	seq = (stp->data[0] >> 3) & 7;
	if (seq == ((stp->rx_seq + 7) & 7))
		return 3;
	if (seq != stp->rx_seq)
		return -EILSEQ;
	stp->rx_seq = (stp->rx_seq + 1) & 7;
	return 1;
}

struct firmware {
	size_t size;
	u8 *data;
};

struct mt6628 {
	struct udevice *dev;
	struct udevice *uart;
	struct clk rtc;
	struct gpio_desc gpios[4];
	struct firmware patch;
	struct firmware calibration;
	struct mt6628_stp stp;
	u8 wire[MT6628_STP_MAX + 6];
	int rx_error;
	u32 hw_version;
	u32 rom_version;
	u8 antenna_mode;
	unsigned int requested;
	bool busy;
	bool draining;
	bool initialized;
	bool powered;
	bool rtc_enabled;
};

static int mt6628_write(struct mt6628 *ctx, const u8 *data, size_t length)
{
	struct dm_serial_ops *ops = serial_get_ops(ctx->uart);
	ulong start = get_timer(0);
	size_t offset = 0;
	int ret;

	while (offset < length) {
		ret = ops->putc(ctx->uart, data[offset]);
		if (ret && ret != -EAGAIN)
			return ret;
		if (!ret)
			offset++;
		if (get_timer(start) >= 1000)
			return -ETIMEDOUT;
		if (ret)
			udelay(1);
	}
	for (;;) {
		ret = ops->pending(ctx->uart, false);
		if (ret <= 0)
			return ret;
		if (get_timer(start) >= 1000)
			return -ETIMEDOUT;
		udelay(1);
	}
}

static int mt6628_exchange(struct mt6628 *ctx, const u8 *command,
			   size_t command_len, u8 *event, size_t event_len,
			   bool raw)
{
	struct dm_serial_ops *ops = serial_get_ops(ctx->uart);
	u8 ack[4];
	size_t received = 0;
	unsigned int attempt;
	ulong start;
	int length, ret, byte;

	if (ctx->rx_error)
		return ctx->rx_error;
	if (!event_len || event_len > 64)
		return -EMSGSIZE;
	if (raw) {
		if (ctx->stp.full || command_len != 1 || command[0] != 0xff)
			return -EINVAL;
		ctx->wire[0] = 0xff;
		length = 1;
	} else {
		length = mt6628_stp_tx(&ctx->stp, command, command_len,
				       ctx->wire, sizeof(ctx->wire));
		if (length < 0)
			return length;
	}
	for (attempt = 0; attempt < 3; attempt++) {
		ret = mt6628_write(ctx, ctx->wire, length);
		if (ret)
			goto fail;
		start = get_timer(0);
		for (;;) {
			byte = ops->getc(ctx->uart);
			if (byte < 0 && byte != -EAGAIN) {
				ret = byte;
				goto fail;
			}
			if (byte >= 0) {
				ret = mt6628_stp_rx(&ctx->stp, byte);
				if (ret < 0)
					goto fail;
				if (ret == 1) {
					if (ctx->stp.length > event_len - received) {
						ret = -EMSGSIZE;
						goto fail;
					}
					memcpy(event + received, ctx->stp.data + 4,
					       ctx->stp.length);
					received += ctx->stp.length;
				}
				if (ctx->stp.full && (ret == 1 || ret == 3)) {
					mt6628_stp_ack(&ctx->stp, ack);
					ret = mt6628_write(ctx, ack, sizeof(ack));
					if (ret)
						goto fail;
				}
			} else if (received == event_len && !ctx->stp.pending &&
				   !ctx->stp.used) {
				return 0;
			}
			if (get_timer(start) >= 1000)
				break;
			if (byte == -EAGAIN)
				udelay(1);
		}
		if (!ctx->stp.full || !ctx->stp.pending)
			break;
	}
	ret = -ETIMEDOUT;
fail:
	ctx->rx_error = ret;
	return ret;
}

static int mt6628_set_baud(struct mt6628 *ctx, unsigned int baud)
{
	struct dm_serial_ops *ops = serial_get_ops(ctx->uart);

	return baud ? ops->setbrg(ctx->uart, baud) : -EINVAL;
}

static int mt6628_command(struct mt6628 *ctx, const u8 *command,
			  size_t length, const u8 *expected, size_t event_len)
{
	u8 event[64];
	int ret;

	if (event_len > sizeof(event))
		return -EMSGSIZE;
	ret = mt6628_exchange(ctx, command, length, event, event_len, false);
	if (ret)
		return ret;
	return memcmp(event, expected, event_len) ? -EBADMSG : 0;
}

static int mt6628_read_reg(struct mt6628 *ctx, u32 address, u32 *value)
{
	u8 command[20] = { 1, 8, 16, 0, 2, 1, 0, 1 };
	u8 event[16];
	int ret;

	put_unaligned_le32(address, command + 8);
	put_unaligned_le32(0xffff, command + 16);
	ret = mt6628_exchange(ctx, command, sizeof(command), event,
			      sizeof(event), false);
	if (ret)
		return ret;
	if (event[0] != 2 || event[1] != 8 ||
	    get_unaligned_le16(event + 2) != 12 || event[4] ||
	    get_unaligned_le32(event + 8) != address)
		return -EBADMSG;
	*value = get_unaligned_le32(event + 12);
	return 0;
}

static int mt6628_download_patch(struct mt6628 *ctx,
				 const struct firmware *firmware)
{
	static const u8 expected[] = { 2, 1, 1, 0, 0 };
	size_t offset, length, payload;
	u16 version;
	u8 *command;
	int ret = 0;

	if (firmware->size <= 28 || firmware->size > SZ_1M)
		return -EINVAL;
	switch (ctx->hw_version) {
	case 0x8a00:
		version = 0x8a00;
		break;
	case 0x8a10:
	case 0x8b10:
	case 0x8b11:
	case 0x8a11:
		version = 0x8a10;
		break;
	default:
		return -ENODEV;
	}
	if (get_unaligned_be16(firmware->data + 20) != version)
		return -EBADMSG;
	command = malloc(1005);
	if (!command)
		return -ENOMEM;
	payload = firmware->size - 28;
	command[0] = 1;
	command[1] = 1;
	for (offset = 0; offset < payload; offset += length) {
		length = min_t(size_t, 1000, payload - offset);
		put_unaligned_le16(length + 1, command + 2);
		command[4] = offset + length == payload ? 3 : offset ? 2 : 1;
		memcpy(command + 5, firmware->data + 28 + offset, length);
		ret = mt6628_command(ctx, command, length + 5, expected,
				     sizeof(expected));
		if (ret)
			break;
	}
	free(command);
	return ret;
}

static int mt6628_crystal_trim(struct mt6628 *ctx,
			       const struct firmware *calibration)
{
	static const u8 query[] = { 1, 0x12, 2, 0, 0, 0 };
	static const u8 prefix[] = { 2, 0x12, 2, 0, 0 };
	u8 command[] = { 1, 0x12, 2, 0, 1, 0 };
	u8 expected[] = { 2, 0x12, 2, 0, 1, 0 };
	u8 event[6];
	int offset, value, ret;

	if (calibration->size < 110)
		return -EINVAL;
	if (!(calibration->data[109] & 0x80))
		return 0;
	offset = calibration->data[109] & 0x7f;
	if (offset & 0x40)
		offset -= 128;
	ret = mt6628_exchange(ctx, query, sizeof(query), event, sizeof(event),
			      false);
	if (ret)
		return ret;
	if (memcmp(event, prefix, sizeof(prefix)))
		return -EBADMSG;
	value = (event[5] & 0x7f) + offset;
	command[5] = value < 0 ? 0 : value;
	expected[5] = command[5];
	ret = mt6628_command(ctx, command, sizeof(command), expected,
			     sizeof(expected));
	if (ret)
		return ret;
	expected[4] = 0;
	return mt6628_command(ctx, query, sizeof(query), expected,
			      sizeof(expected));
}

static const char *mt6628_patch_name(u32 version)
{
	switch (version) {
	case 0x8a00:
		return "mediatek/mt6628_patch_e1_hdr.bin";
	case 0x8a10:
	case 0x8b10:
	case 0x8b11:
	case 0x8a11:
		return "mediatek/mt6628_patch_e2_hdr.bin";
	default:
		return NULL;
	}
}

static int mt6628_identify(struct mt6628 *ctx)
{
	u32 chip, version, rom;
	int ret;

	ret = mt6628_read_reg(ctx, 0x80000008, &chip);
	if (ret)
		return ret;
	if (chip != 0x6628) {
		dev_err(ctx->dev, "unsupported chip %#x\n", chip);
		return -ENODEV;
	}
	ret = mt6628_read_reg(ctx, 0x80000000, &version);
	if (ret)
		return ret;
	if (!mt6628_patch_name(version)) {
		dev_err(ctx->dev, "unsupported MT6628 revision %#x\n", version);
		return -ENODEV;
	}
	ret = mt6628_read_reg(ctx, 0x80000004, &rom);
	if (ret)
		return ret;
	ctx->hw_version = version;
	ctx->rom_version = rom;
	dev_info(ctx->dev, "MT6628 hw %#x ROM %#x\n", version, rom);
	return 0;
}

static int mt6628_query(struct mt6628 *ctx, u8 selector, u32 value)
{
	u8 command[] = { 1, 4, 1, 0, selector };
	u8 expected[] = { 2, 4, 6, 0, 0, selector, 0, 0, 0, 0 };

	put_unaligned_le32(value, expected + 6);
	return mt6628_command(ctx, command, sizeof(command), expected,
			      sizeof(expected));
}

static int mt6628_bootstrap(struct mt6628 *ctx, const struct firmware *patch,
			    const struct firmware *calibration, u8 antenna_mode)
{
	static const u8 baud[] = { 1, 4, 5, 0, 1, 0, 9, 0x3d, 0 };
	static const u8 baud_event[] = { 2, 4, 2, 0, 0, 1 };
	static const u8 wake[] = { 0xff };
	static const u8 wake_event[] = { 2, 3, 2, 0, 0, 3 };
	static const u8 reset[] = { 1, 7, 1, 0, 4 };
	static const u8 reset_event[] = { 2, 7, 1, 0, 0 };
	static const u8 coex_event[] = { 2, 0x10, 1, 0, 0 };
	static const u8 full[] = { 1, 4, 5, 0, 3, 0xdf, 0x0e, 0x68, 1 };
	static const u8 full_event[] = { 2, 4, 2, 0, 0, 3 };
	u8 coex[] = { 1, 0x10, 2, 0, 1, antenna_mode };
	u8 event[sizeof(wake_event)];
	int ret;

	if (antenna_mode < 1 || antenna_mode > 4)
		return -EINVAL;
	ret = mt6628_query(ctx, 2, 115200);
	if (ret)
		return ret;
	ret = mt6628_query(ctx, 4, 0x11);
	if (ret)
		return ret;
	ret = mt6628_command(ctx, baud, sizeof(baud), baud_event,
			     sizeof(baud_event));
	if (ret)
		return ret;
	ret = mt6628_set_baud(ctx, 4000000);
	if (ret)
		return ret;
	ret = mt6628_exchange(ctx, wake, sizeof(wake), event, sizeof(event), true);
	if (ret)
		return ret;
	if (memcmp(event, wake_event, sizeof(event)))
		return -EBADMSG;
	ret = mt6628_query(ctx, 2, 4000000);
	if (ret)
		return ret;
	ret = mt6628_download_patch(ctx, patch);
	if (ret)
		return ret;
	ret = mt6628_command(ctx, reset, sizeof(reset), reset_event,
			     sizeof(reset_event));
	if (ret)
		return ret;
	ret = mt6628_command(ctx, coex, sizeof(coex), coex_event,
			     sizeof(coex_event));
	if (ret)
		return ret;
	ret = mt6628_crystal_trim(ctx, calibration);
	if (ret)
		return ret;
	ret = mt6628_command(ctx, full, sizeof(full), full_event,
			     sizeof(full_event));
	if (ret)
		return ret;
	if (ctx->rx_error)
		return ctx->rx_error;
	if (ctx->stp.pending || ctx->stp.used)
		return -EBUSY;
	ctx->stp = (struct mt6628_stp) { .full = true };
	mdelay(10);
	ret = mt6628_query(ctx, 4, 0x01680edf);
	if (ret)
		return ret;
	ret = mt6628_query(ctx, 2, 4000000);
	if (ret)
		return ret;
	return ctx->rx_error;
}

static int mt6628_load_firmware(struct mt6628 *ctx, const char *name,
				struct firmware *firmware, size_t minimum,
				size_t maximum)
{
	struct udevice *loader;
	u8 *data;
	int ret;

	if (firmware->data)
		return 0;
	ret = get_fs_loader(&loader);
	if (ret)
		return ret;
	data = malloc(maximum + 1);
	if (!data)
		return -ENOMEM;
	ret = request_firmware_into_buf(loader, name, data, maximum + 1, 0);
	if (ret < 0 || (size_t)ret < minimum || (size_t)ret > maximum) {
		free(data);
		return ret < 0 ? ret : -EINVAL;
	}
	firmware->data = data;
	firmware->size = ret;
	return 0;
}

static int mt6628_power_off(struct mt6628 *ctx)
{
	static const unsigned int controls[] = { 0, 2, 3 };
	struct dm_serial_ops *ops = serial_get_ops(ctx->uart);
	unsigned int i;
	int ret = 0, err;

	ctx->powered = false;
	ctx->rx_error = -ESHUTDOWN;
	for (i = 0; i < ARRAY_SIZE(controls); i++) {
		err = dm_gpio_set_value(&ctx->gpios[controls[i]], !i);
		if (err && !ret)
			ret = err;
	}
	err = ops->clear(ctx->uart);
	if (err && !ret)
		ret = err;
	err = pinctrl_select_state(ctx->dev, "default");
	if (err && !ret)
		ret = err;
	if (ctx->rtc_enabled) {
		err = clk_disable(&ctx->rtc);
		if (!err)
			ctx->rtc_enabled = false;
		if (err && !ret)
			ret = err;
	}
	if (!ret)
		ret = dm_gpio_set_value(&ctx->gpios[1], 0);
	return ret;
}

static int mt6628_power_on(struct mt6628 *ctx)
{
	struct dm_serial_ops *ops = serial_get_ops(ctx->uart);
	unsigned int i;
	int ret, err;

	if (ctx->powered && !ctx->rx_error)
		return 0;
	ret = mt6628_power_off(ctx);
	if (ret)
		return ret;
	ret = mt6628_load_firmware(ctx, "mediatek/mt6628-nvram.bin",
				   &ctx->calibration, 110, 4096);
	if (ret)
		return ret;
	ret = dm_gpio_set_value(&ctx->gpios[1], 1);
	if (ret)
		goto off;
	mdelay(100);
	ret = clk_enable(&ctx->rtc);
	if (ret)
		goto off;
	ctx->rtc_enabled = true;
	mdelay(100);
	ret = pinctrl_select_state(ctx->dev, "active");
	if (ret)
		goto off;
	ret = mt6628_set_baud(ctx, 115200);
	if (ret)
		goto off;
	ret = ops->clear(ctx->uart);
	if (ret)
		goto off;
	ctx->stp = (struct mt6628_stp) { 0 };
	ctx->rx_error = 0;
	mdelay(10);
	for (i = 2; i < ARRAY_SIZE(ctx->gpios); i++) {
		ret = dm_gpio_set_value(&ctx->gpios[i], 1);
		if (ret)
			goto off;
	}
	mdelay(30);
	ret = dm_gpio_set_value(&ctx->gpios[0], 0);
	if (ret)
		goto off;
	mdelay(30);
	ret = mt6628_identify(ctx);
	if (ret)
		goto off;
	ret = mt6628_load_firmware(ctx, mt6628_patch_name(ctx->hw_version),
				   &ctx->patch, 29, SZ_1M);
	if (ret)
		goto off;
	ret = mt6628_bootstrap(ctx, &ctx->patch, &ctx->calibration,
			       ctx->antenna_mode);
	if (ret)
		goto off;
	ctx->powered = true;
	return 0;
off:
	err = mt6628_power_off(ctx);
	if (err)
		dev_err(ctx->dev, "power cleanup failed: %d\n", err);
	return ret;
}

static int mt6628_set_power(struct udevice *dev, bool enable)
{
	struct mt6628 *ctx = dev_get_plat(dev);
	int ret;

	if (ctx->draining)
		return enable ? -EBUSY : 0;
	if (ctx->busy)
		return -EBUSY;
	if (!ctx->initialized)
		return -ENODEV;
	ctx->busy = true;
	ret = enable ? mt6628_power_on(ctx) : mt6628_power_off(ctx);
	ctx->busy = false;
	return ret;
}

static int mt6628_probe(struct udevice *dev)
{
	static const char * const names[] = {
		"reset-gpios", "enable-gpios", "power-gpios", "power-gpios",
	};
	struct mt6628 *ctx = dev_get_plat(dev);
	struct dm_serial_ops *ops;
	u32 antenna_mode;
	unsigned int i;
	int ret;

	if (ctx->busy)
		return -EBUSY;
	if (ctx->initialized)
		return 0;
	ctx->busy = true;
	ctx->dev = dev;
	ctx->uart = dev->parent;
	if (gd->cur_serial_dev == ctx->uart) {
		ret = -EBUSY;
		goto out;
	}
	if (device_get_uclass_id(ctx->uart) != UCLASS_SERIAL) {
		ret = -EINVAL;
		goto out;
	}
	ops = serial_get_ops(ctx->uart);
	if (!ops->putc || !ops->getc || !ops->pending || !ops->setbrg ||
	    !ops->clear) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	ret = dev_read_u32(dev, "mediatek,antenna-mode", &antenna_mode);
	if (ret)
		goto out;
	if (antenna_mode < 1 || antenna_mode > 4) {
		ret = -EINVAL;
		goto out;
	}
	ctx->antenna_mode = antenna_mode;
	ret = gpio_get_list_count(dev, "power-gpios");
	if (ret != 2) {
		ret = ret < 0 ? ret : -EINVAL;
		goto out;
	}
	ret = clk_get_by_name(dev, "rtc", &ctx->rtc);
	if (ret)
		goto out;
	ret = pinctrl_select_state(dev, "default");
	if (ret)
		goto out;
	for (i = ctx->requested; i < ARRAY_SIZE(ctx->gpios); i++) {
		ret = gpio_request_by_name(dev, names[i], i == 3,
					   &ctx->gpios[i], GPIOD_IS_OUT |
					   (!i ? GPIOD_IS_OUT_ACTIVE : 0));
		if (ret)
			goto out;
		ctx->requested++;
	}
	ctx->initialized = true;
out:
	ctx->busy = false;
	return ret;
}

static int mt6628_remove(struct udevice *dev)
{
	struct mt6628 *ctx = dev_get_plat(dev);
	struct udevice *consumer;
	struct uclass *uc;
	int ret;

	if (ctx->busy || ctx->draining)
		return -EBUSY;
	ret = uclass_get(UCLASS_MMC, &uc);
	if (ret)
		return ret;
	ctx->draining = true;
	uclass_foreach_dev(consumer, uc) {
		struct mmc *mmc = mmc_get_mmc_dev(consumer);

		if (!mmc || mmc->cfg->pwr_dev != dev)
			continue;
		ret = device_remove(consumer, DM_REMOVE_NORMAL);
		if (ret) {
			ctx->draining = false;
			return ret;
		}
	}
	ctx->draining = false;
	if (ctx->initialized) {
		ret = mt6628_set_power(dev, false);
		if (ret)
			return ret;
		ctx->initialized = false;
	}
	free(ctx->patch.data);
	ctx->patch = (struct firmware) { 0 };
	free(ctx->calibration.data);
	ctx->calibration = (struct firmware) { 0 };
	return 0;
}

static int mt6628_unbind(struct udevice *dev)
{
	struct mt6628 *ctx = dev_get_plat(dev);
	int ret;

	ret = mt6628_remove(dev);
	if (ret)
		return ret;
	ctx->initialized = false;
	while (ctx->requested) {
		ret = dm_gpio_free(dev, &ctx->gpios[ctx->requested - 1]);
		if (ret)
			return ret;
		ctx->requested--;
	}
	return 0;
}

static const struct pwrseq_ops mt6628_ops = {
	.set_power = mt6628_set_power,
};

static const struct udevice_id mt6628_ids[] = {
	{ .compatible = "mediatek,mt6628-wmt" },
	{ }
};

U_BOOT_DRIVER(mt6628_wmt) = {
	.name = "mt6628-wmt",
	.id = UCLASS_PWRSEQ,
	.of_match = mt6628_ids,
	.ops = &mt6628_ops,
	.probe = mt6628_probe,
	.remove = mt6628_remove,
	.unbind = mt6628_unbind,
	.plat_auto = sizeof(struct mt6628),
	.flags = DM_FLAG_OS_PREPARE,
};
