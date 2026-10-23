/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __BEGONIA_MEMORY_H
#define __BEGONIA_MEMORY_H

#include <linux/types.h>

struct begonia_region {
	u64 start;
	u64 size;
};

int begonia_free_memory(const struct begonia_region *banks, unsigned int nbanks,
			const struct begonia_region *reserved, unsigned int nreserved,
			int (*visit)(u64 start, u64 size, void *arg), void *arg);

#endif
