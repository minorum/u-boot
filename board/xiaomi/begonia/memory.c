// SPDX-License-Identifier: GPL-2.0+
#include <linux/errno.h>
#include "memory.h"

int begonia_free_memory(const struct begonia_region *banks, unsigned int nbanks,
			const struct begonia_region *reserved, unsigned int nreserved,
			int (*visit)(u64 start, u64 size, void *arg), void *arg)
{
	u64 cursor, end, next, covered, limit;
	unsigned int i, j;
	int ret;

	for (i = 0; i < nbanks; i++) {
		if (!banks[i].size || banks[i].size > ~banks[i].start)
			return -EINVAL;
		end = banks[i].start + banks[i].size;
		for (j = 0; j < i; j++) {
			if (banks[i].start < banks[j].start + banks[j].size &&
			    banks[j].start < end)
				return -EINVAL;
		}
	}
	for (i = 0; i < nreserved; i++) {
		if (reserved[i].size > ~reserved[i].start)
			return -EINVAL;
	}
	for (i = 0; i < nbanks; i++) {
		cursor = banks[i].start;
		end = cursor + banks[i].size;
		while (cursor < end) {
			next = end;
			covered = cursor;
			for (j = 0; j < nreserved; j++) {
				if (!reserved[j].size)
					continue;
				limit = reserved[j].start + reserved[j].size;
				if (reserved[j].start <= cursor && limit > covered)
					covered = limit;
				if (reserved[j].start > cursor && reserved[j].start < next)
					next = reserved[j].start;
			}
			if (covered > cursor) {
				cursor = covered;
				continue;
			}
			ret = visit(cursor, next - cursor, arg);
			if (ret)
				return ret;
			cursor = next;
		}
	}
	return 0;
}
