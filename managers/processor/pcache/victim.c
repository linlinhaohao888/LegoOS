/*
 * Copyright (c) 2016-2017 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * Our Victim Cache Friend
 */

#include <lego/mm.h>
#include <lego/wait.h>
#include <lego/slab.h>
#include <lego/log2.h>
#include <lego/hash.h>
#include <lego/kernel.h>
#include <lego/pgfault.h>
#include <lego/jiffies.h>
#include <lego/syscalls.h>
#include <lego/memblock.h>
#include <processor/pcache.h>
#include <processor/processor.h>

struct pcache_victim_meta *pcache_victim_meta_map;
void *pcache_victim_data_map;

void dump_pcache_victim(struct pcache_victim_meta *pvm, const char *reason)
{

}

static inline struct pcache_victim_meta *
victim_alloc_fastpath(void)
{
	int index;
	struct pcache_victim_meta *v;

	for_each_victim(v, index) {
		if (likely(!TestSetVictimAllocated(v))) {
			return v;
		}
	}
	return NULL;
}

static struct pcache_victim_meta *
victim_alloc_slowpath(void)
{
	return NULL;
}

static struct pcache_victim_meta *victim_alloc(void)
{
	struct pcache_victim_meta *v;

	v = victim_alloc_fastpath();
	if (likely(v))
		goto out;

	v = victim_alloc_slowpath();
	if (likely(v))
		goto out;
	return NULL;

out:
	/* May need further initilization if needed */
	return v;
}

static void victim_free(struct pcache_victim_meta *v)
{
	PCACHE_BUG_ON_VICTIM(!VictimAllocated(v) || VictimLocked(v) ||
			      VictimWriteback(v), v);

	ClearVictimAllocated(v);
}

void victim_prepare_insert(struct pcache_set *pset, struct pcache_meta *pcm)
{

}

void victim_finish_insert(struct pcache_meta *pcm)
{

}

/*
 * Allocate victim metadata and cache lines
 */
void __init pcache_init_victim_cache(void)
{
	u64 size;
	int i;

	size = VICTIM_NR_ENTRIES * PCACHE_LINE_SIZE;
	pcache_victim_data_map = memblock_virt_alloc(size, PAGE_SIZE);
	if (!pcache_victim_data_map)
		panic("Unable to allocate victim data map!");
	memset(pcache_victim_data_map, 0, size);

	size = VICTIM_NR_ENTRIES * sizeof(struct pcache_victim_meta);
	pcache_victim_meta_map = memblock_virt_alloc(size, PAGE_SIZE);
	if (!pcache_victim_meta_map)
		panic("Unable to allocate victim meta map!");

	for (i = 0; i < VICTIM_NR_ENTRIES; i++) {
		struct pcache_victim_meta *v;

		v = pcache_victim_meta_map + i;
		v->flags = 0;
		v->pcm = NULL;
	}
}
