// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2022 Sean Anderson <sean.anderson@seco.com>
 */

#include <i2c_eeprom.h>
#include <linker_lists.h>
#include <malloc.h>
#include <misc.h>
#include <nvmem.h>
#include <rtc.h>
#include <dm/device_compat.h>
#include <dm/ofnode.h>
#include <dm/read.h>
#include <dm/uclass.h>

static int nvmem_cell_read_raw(struct nvmem_cell *cell, void *buf, size_t size)
{
	dev_dbg(cell->nvmem, "%s: off=%u size=%zu\n", __func__, cell->offset, size);
	if (size != cell->size)
		return -EINVAL;

	switch (cell->nvmem->driver->id) {
	case UCLASS_I2C_EEPROM:
		return i2c_eeprom_read(cell->nvmem, cell->offset, buf, size);
	case UCLASS_MISC: {
		int ret = misc_read(cell->nvmem, cell->offset, buf, size);

		if (ret < 0)
			return ret;
		if (ret != size)
			return -EIO;
		return 0;
	}
	case UCLASS_RTC:
		return dm_rtc_read(cell->nvmem, cell->offset, buf, size);
	default:
		return -ENOSYS;
	}
}

static int nvmem_cell_write_raw(struct nvmem_cell *cell, const void *buf, size_t size)
{
	dev_dbg(cell->nvmem, "%s: off=%u size=%zu\n", __func__, cell->offset, size);
	if (size != cell->size)
		return -EINVAL;

	switch (cell->nvmem->driver->id) {
	case UCLASS_I2C_EEPROM:
		return i2c_eeprom_write(cell->nvmem, cell->offset, buf, size);
	case UCLASS_MISC: {
		int ret = misc_write(cell->nvmem, cell->offset, buf, size);

		if (ret < 0)
			return ret;
		if (ret != size)
			return -EIO;
		return 0;
	}
	case UCLASS_RTC:
		return dm_rtc_write(cell->nvmem, cell->offset, buf, size);
	default:
		return -ENOSYS;
	}
}

int nvmem_cell_read(struct nvmem_cell *cell, void *buf, size_t size)
{
	u8 *data = buf;
	u64 bit, pos;
	size_t used;
	int ret;

	ret = nvmem_cell_read_raw(cell, buf, size);
	if (ret || !cell->nbits)
		return ret;
	for (bit = 0; bit < cell->nbits; bit++) {
		pos = bit + cell->bit_offset;
		if (data[pos / 8] & BIT(pos % 8))
			data[bit / 8] |= BIT(bit % 8);
		else
			data[bit / 8] &= ~BIT(bit % 8);
	}
	used = DIV_ROUND_UP((u64)cell->nbits, 8);
	if (cell->nbits % 8)
		data[used - 1] &= BIT(cell->nbits % 8) - 1;
	memset(data + used, 0, size - used);
	return 0;
}

int nvmem_cell_write(struct nvmem_cell *cell, const void *buf, size_t size)
{
	const u8 *data = buf;
	u8 *raw;
	u64 bit, pos;
	int ret;

	if (size != cell->size)
		return -EINVAL;
	if (!cell->nbits)
		return nvmem_cell_write_raw(cell, buf, size);
	raw = malloc(size);
	if (!raw)
		return -ENOMEM;
	ret = nvmem_cell_read_raw(cell, raw, size);
	if (ret)
		goto out;
	for (bit = 0; bit < cell->nbits; bit++) {
		pos = bit + cell->bit_offset;
		if (data[bit / 8] & BIT(bit % 8))
			raw[pos / 8] |= BIT(pos % 8);
		else
			raw[pos / 8] &= ~BIT(pos % 8);
	}
	ret = nvmem_cell_write_raw(cell, raw, size);
out:
	free(raw);
	return ret;
}

/**
 * nvmem_get_device() - Get an nvmem device for a cell
 * @node: ofnode of the nvmem device
 * @cell: Cell to look up
 *
 * Try to find a nvmem-compatible device by going through the nvmem interfaces.
 *
 * Return:
 * * 0 on success
 * * -ENODEV if we didn't find anything
 * * A negative error if there was a problem looking up the device
 */
static int nvmem_get_device(ofnode node, struct nvmem_cell *cell)
{
	int i, ret;
	enum uclass_id ids[] = {
		UCLASS_I2C_EEPROM,
		UCLASS_MISC,
		UCLASS_RTC,
	};

	for (i = 0; i < ARRAY_SIZE(ids); i++) {
		ret = uclass_get_device_by_ofnode(ids[i], node, &cell->nvmem);
		if (!ret)
			return 0;
		if (ret != -ENODEV && ret != -EPFNOSUPPORT)
			return ret;
	}

	return -ENODEV;
}

int nvmem_cell_get_by_index(struct udevice *dev, int index,
			    struct nvmem_cell *cell)
{
	fdt_addr_t offset;
	fdt_size_t size = FDT_SIZE_T_NONE;
	int ret;
	struct ofnode_phandle_args args;
	const fdt32_t *bits;
	u64 bytes;
	int len;
	ofnode par;

	dev_dbg(dev, "%s: index=%d\n", __func__, index);

	ret = dev_read_phandle_with_args(dev, "nvmem-cells", NULL, 0, index,
					 &args);
	if (ret)
		return ret;

	par = ofnode_get_parent(args.node);
	if (ofnode_device_is_compatible(par, "fixed-layout"))
		par = ofnode_get_parent(par);

	ret = nvmem_get_device(par, cell);
	if (ret)
		return ret;

	offset = ofnode_get_addr_size_index_notrans(args.node, 0, &size);
	if (offset == FDT_ADDR_T_NONE || size == FDT_SIZE_T_NONE) {
		dev_dbg(cell->nvmem, "missing address or size for %s\n",
			ofnode_get_name(args.node));
		return -EINVAL;
	}

	cell->offset = offset;
	cell->size = size;
	cell->bit_offset = 0;
	cell->nbits = 0;
	if (ofnode_has_property(args.node, "bits")) {
		bits = ofnode_get_property(args.node, "bits", &len);
		if (!bits || len != 2 * sizeof(*bits))
			return -EINVAL;
		cell->bit_offset = fdt32_to_cpu(bits[0]);
		cell->nbits = fdt32_to_cpu(bits[1]);
		bytes = DIV_ROUND_UP((u64)cell->bit_offset + cell->nbits, 8);
		if (!cell->nbits || bytes > size)
			return -EINVAL;
		cell->size = bytes;
	}
	return 0;
}

int nvmem_cell_get_by_name(struct udevice *dev, const char *name,
			   struct nvmem_cell *cell)
{
	int index;

	dev_dbg(dev, "%s, name=%s\n", __func__, name);

	index = dev_read_stringlist_search(dev, "nvmem-cell-names", name);
	if (index < 0)
		return index;

	return nvmem_cell_get_by_index(dev, index, cell);
}
