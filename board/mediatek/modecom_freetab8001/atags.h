/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MODECOM_ATAGS_H
#define __MODECOM_ATAGS_H

#include <linux/errno.h>
#include <linux/types.h>

#define MODECOM_RAM_BANKS 4
#define MODECOM_MIN_BOOT_BANK 0x04000000U

extern u32 modecom_boot_atags;

struct modecom_ram {
	u32 start[MODECOM_RAM_BANKS];
	u32 size[MODECOM_RAM_BANKS];
	u32 count;
};

static inline int modecom_ram_contains(const struct modecom_ram *ram, u32 start, u32 size)
{
	u32 end;

	if (!ram || !ram->count || ram->count > MODECOM_RAM_BANKS || !size)
		return 0;
	end = ram->start[ram->count - 1] + ram->size[ram->count - 1];
	return start >= ram->start[0] && start < end && size <= end - start;
}

static inline int modecom_parse_atags(const u32 *tags, u32 words,
				      struct modecom_ram *ram)
{
	u32 pos, length, type, start, size, index;
	u32 fb_start = 0, fb_size = 0, fb_seen = 0;

	if (!tags || !ram || words < 2 || words > 1024 || tags[1] != 0x54410001)
		return -EINVAL;
	*ram = (struct modecom_ram){0};
	for (pos = 0; pos + 2 <= words; pos += length) {
		length = tags[pos];
		type = tags[pos + 1];
		if (!type) {
			if (length || !ram->count || !fb_seen)
				return -EINVAL;
			index = ram->count - 1;
			start = ram->start[index];
			size = ram->size[index];
			if (fb_start || fb_size) {
				if (!fb_size || (fb_start | fb_size) & 0xfff ||
				    fb_start <= start || fb_start >= start + size ||
				    fb_size > start + size - fb_start)
					return -EINVAL;
				ram->size[index] = fb_start - start;
			}
			if (ram->size[index] < MODECOM_MIN_BOOT_BANK)
				return -EINVAL;
			return 0;
		}
		if (length < 2 || length > words - pos)
			return -EINVAL;
		if (type == 0x54410002) {
			if (length != 4 || ram->count == MODECOM_RAM_BANKS)
				return -EINVAL;
			size = tags[pos + 2];
			start = tags[pos + 3];
			if (!size || start < 0x01600000 || start >= 0x40000000 ||
			    size > 0x40000000 - start || (start | size) & 0xfff)
				return -EINVAL;
			index = ram->count;
			if (index && start != ram->start[index - 1] + ram->size[index - 1])
				return -EINVAL;
			ram->start[index] = start;
			ram->size[index] = size;
			ram->count++;
		} else if (type == 0x54410008) {
			if (length != 8 || fb_seen)
				return -EINVAL;
			fb_start = tags[pos + 4];
			fb_size = tags[pos + 5];
			fb_seen = 1;
		}
	}
	return -EINVAL;
}

#endif
