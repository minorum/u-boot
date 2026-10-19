// SPDX-License-Identifier: GPL-2.0-or-later
#include <command.h>
#include <env.h>
#include <fs.h>
#include <linux/errno.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <asm/unaligned.h>

#define KERNEL_ADDR 0x80008000UL
#define FDT_ADDR 0x85400000UL
#define INITRD_ADDR 0x85500000UL
#define PAGE_SIZE 2048U
#define BOARD_REV 5U

int j320fn_guard(void);

static int boot_read(const char *filename, ulong address, u32 offset, u32 size)
{
	loff_t actual;

	if (fs_set_blk_dev("mmc", "0#pmOS_boot", FS_TYPE_EXT) ||
	    fs_read(filename, address, offset, size, &actual) ||
	    actual != size)
		return -EIO;
	return 0;
}

static int boot_dtb(u32 size)
{
	u8 *table = (void *)FDT_ADDR;
	u32 count, index, offset = 0, length = 0;
	const fdt32_t *id;
	int property_len, matches = 0;

	if (size < 12 || get_unaligned_le32(table) != 0x44525053 ||
	    get_unaligned_le32(table + 4) != 1)
		return -EINVAL;
	count = get_unaligned_le32(table + 8);
	if (!count || count > 64 || 12 + 20 * count > size)
		return -EINVAL;
	for (index = 0; index < count; index++) {
		u8 *entry = table + 12 + 20 * index;
		u32 start = get_unaligned_le32(entry + 12);
		u32 bytes = get_unaligned_le32(entry + 16);

		if (start < 12 + 20 * count || start > size ||
		    bytes < sizeof(struct fdt_header) || bytes > size - start)
			return -EINVAL;
		if (get_unaligned_le32(entry) == 9830 &&
		    get_unaligned_le32(entry + 4) == BOARD_REV &&
		    get_unaligned_le32(entry + 8) == 0x20000) {
			offset = start;
			length = bytes;
			matches++;
		}
	}
	if (matches != 1 || fdt_check_full(table + offset, length) ||
	    fdt_node_check_compatible(table + offset, 0, "samsung,j3xnlte"))
		return -EINVAL;
	id = fdt_getprop(table + offset, 0, "sprd,sc-id", &property_len);
	if (!id || property_len != 12 || fdt32_to_cpu(id[0]) != 9830 ||
	    fdt32_to_cpu(id[1]) != BOARD_REV ||
	    fdt32_to_cpu(id[2]) != 0x20000)
		return -EINVAL;
	memmove(table, table + offset, fdt_totalsize(table + offset));
	return 0;
}

static int do_j320fn_boot(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	u8 header[PAGE_SIZE];
	const char *filename = argc == 2 ? argv[1] : "/kernels/normal.img";
	char bootargs[1568], command[96];
	loff_t file_size;
	u32 kernel, initrd, dt, initrd_offset, dt_offset;
	u32 zi_start, zi_end;
	size_t first, extra;
	int ret;
	const char *mode = env_get("reboot-mode");
	bool bootloader = argc == 1 && mode && !strcmp(mode, "bootloader");

	if (argc == 1 && mode && !strcmp(mode, "recovery"))
		filename = "/kernels/recovery.img";
	env_set("reboot-mode", NULL);
	if (bootloader)
		return CMD_RET_FAILURE;
	if (IS_ENABLED(CONFIG_EFI_LOADER) && argc == 1 &&
	    !strcmp(filename, "/kernels/normal.img"))
		run_command("ext2load mmc 0#pmOS_boot 81000000 "
			    "/EFI/BOOT/BOOTARM.EFI && bootefi 81000000", 0);

	if (fs_set_blk_dev("mmc", "0#pmOS_boot", FS_TYPE_EXT) ||
	    fs_size(filename, &file_size) ||
	    file_size < PAGE_SIZE || file_size > SZ_32M ||
	    boot_read(filename, (ulong)header, 0, sizeof(header)))
		goto fail;
	if (memcmp(header, "ANDROID!", 8) ||
	    get_unaligned_le32(header + 24) ||
	    get_unaligned_le32(header + 36) != PAGE_SIZE ||
	    get_unaligned_le32(header + 44))
		goto fail;
	kernel = get_unaligned_le32(header + 8);
	initrd = get_unaligned_le32(header + 16);
	dt = get_unaligned_le32(header + 40);
	if (kernel < 48 || kernel > SZ_16M || initrd < 18 ||
	    initrd > SZ_16M || dt < 12 || dt > SZ_1M)
		goto fail;
	initrd_offset = PAGE_SIZE + ALIGN(kernel, PAGE_SIZE);
	dt_offset = initrd_offset + ALIGN(initrd, PAGE_SIZE);
	if ((u64)dt_offset + dt > file_size)
		goto fail;
	first = strnlen((char *)header + 64, 512);
	extra = strnlen((char *)header + 608, 1024);
	if (extra == 1024 || !first || (first < 512 && extra))
		goto fail;
	memcpy(bootargs, header + 64, first);
	memcpy(bootargs + first, header + 608, extra);
	bootargs[first + extra] = 0;
	strlcat(bootargs, " j320fn.bootloader=u-boot", sizeof(bootargs));
	if (boot_read(filename, KERNEL_ADDR, PAGE_SIZE, kernel) ||
	    boot_read(filename, INITRD_ADDR, initrd_offset, initrd) ||
	    boot_read(filename, FDT_ADDR, dt_offset, dt) || boot_dtb(dt))
		goto fail;
	zi_start = get_unaligned_le32((void *)(KERNEL_ADDR + 40));
	zi_end = get_unaligned_le32((void *)(KERNEL_ADDR + 44));
	if (get_unaligned_le32((void *)(KERNEL_ADDR + 36)) != 0x016f2818 ||
	    zi_end <= zi_start || zi_end - zi_start > kernel ||
	    get_unaligned_le16((void *)INITRD_ADDR) != 0x8b1f ||
	    env_set("bootargs", bootargs))
		goto fail;
	if (strcmp(filename, "/kernels/normal.img") && j320fn_guard())
		goto fail;
	snprintf(command, sizeof(command), "bootz %lx %lx:%x %lx",
		 KERNEL_ADDR, INITRD_ADDR, initrd, FDT_ADDR);
	ret = run_command(command, 0);
	if (!ret)
		return ret;
fail:
	puts("J320FN Linux image rejected\n");
	if (argc == 1 && strcmp(filename, "/kernels/normal.img")) {
		puts("J320FN falling back to normal kernel\n");
		return do_j320fn_boot(cmdtp, flag, argc, argv);
	}
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(j320fn_boot, 2, 0, do_j320fn_boot,
	   "boot a J320FN Linux image", "[image path in pmOS_boot]");
