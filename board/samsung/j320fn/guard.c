// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef GUARD_STANDALONE
#include <command.h>
#endif

typedef unsigned int guard_u32;

int j320fn_guard(void);

static guard_u32 guard_read(guard_u32 address)
{
	return *(volatile guard_u32 *)address;
}

static void guard_write(guard_u32 address, guard_u32 value)
{
	*(volatile guard_u32 *)address = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

static int guard_adi_read(guard_u32 offset, guard_u32 *value)
{
	guard_u32 config = guard_read(0x4003001c);
	guard_u32 low = config & 63, shift = (config >> 11) & 31;
	guard_u32 width, mask, count, data;

	if (low < shift || low - shift >= 15)
		return -1;
	width = low - shift + 1;
	mask = (1U << width) - 1;
	guard_write(0x40030024, 0x40038000 + offset);
	for (count = 0; count < 20000; count++) {
		data = guard_read(0x40030028);
		if (data & (1U << 31))
			continue;
		if (((data >> 16) & mask) != ((0x40038000 + offset) & mask))
			return -1;
		*value = data & 0xffff;
		return 0;
	}
	return -1;
}

static int guard_adi_write(guard_u32 offset, guard_u32 value)
{
	unsigned int count;

	for (count = 0; count < 20000; count++)
		if (guard_read(0x4003002c) & (1U << 10))
			break;
	if (count == 20000)
		return -1;
	guard_write(0x40038000 + offset, value);
	for (count = 0; count < 20000; count++)
		if (guard_read(0x4003002c) & (1U << 10))
			return 0;
	return -1;
}

static int guard_update(guard_u32 offset, guard_u32 clear, guard_u32 set)
{
	guard_u32 value;

	if (guard_adi_read(offset, &value))
		return -1;
	return guard_adi_write(offset, (value & ~clear) | set);
}

int j320fn_guard(void)
{
	guard_u32 value, version;
	unsigned int count;
	int ret = -1;

	guard_write(0x402e1000, (1U << 16) | (1U << 22));
	version = guard_read(0x40060ffc);
	if (version != 0x100 && version != 0x200 && version != 0x300)
		return -1;
	for (count = 0; count < 20000; count++)
		if (!guard_read(0x40060800))
			break;
	if (count == 20000)
		return -1;
	if (guard_adi_read(0x940, &value) || value != 0x2723)
		goto out;
	if (guard_adi_read(0x48, &value))
		goto out;
	if (value & 0xb) {
		guard_u32 high, low;

		if (value == 0xe && !guard_adi_read(0x44, &high) &&
		    !guard_adi_read(0x40, &low) && high == 90 && !low)
			ret = 0;
		goto out;
	}
	if (guard_update(0x800, 0, 4) || guard_update(0x808, 0, 4) ||
	    guard_adi_write(0x60, 0xe551) || guard_update(0x48, 0xf, 4) ||
	    guard_adi_write(0x44, 90) || guard_adi_write(0x40, 0))
		goto relock;
	for (count = 0; count < 20000; count++) {
		if (guard_adi_read(0x50, &value))
			goto relock;
		if (!(value & 0x10))
			break;
	}
	if (count == 20000 || guard_update(0x48, 0xf, 0xe))
		goto relock;
	if (!guard_adi_read(0x48, &value) && value == 0xe)
		ret = 0;
relock:
	if (guard_adi_write(0x60, 0x1aae))
		ret = -1;
out:
	guard_write(0x40060800, 0x55aa10c5);
	return ret;
}

#ifdef GUARD_STANDALONE
int guard_entry(int argc, char *const argv[])
{
	int ret = j320fn_guard();

	guard_write(0x8510f000, ret ? 0xbad00001 : 0x272300b4);
	return ret;
}
#else
static int do_j320fn_guard(struct cmd_tbl *cmdtp, int flag, int argc,
			   char *const argv[])
{
	int ret = j320fn_guard();

	printf("J320FN 180-second watchdog guard: %s\n", ret ? "failed" : "armed");
	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(j320fn_guard, 1, 0, do_j320fn_guard,
	   "arm the J320FN trial watchdog", "");
#endif
