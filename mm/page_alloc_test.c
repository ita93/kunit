#include <linux/errname.h>
#include <linux/list.h>
#include <linux/gfp.h>
#include <linux/memory.h>
#include <linux/nodemask.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <linux/memory_hotplug.h>

#include <kunit/test.h>
#include "internal.h"

#define EXPECT_PCPLIST_EMPTY(test, zone, cpu, pindex) ({			\
	struct per_cpu_pages *pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);	\
	struct page *page;							\
										\
	lockdep_assert_held(&pcp->lock);					\
	page = list_first_entry_or_null(					\
			&pcp->lists[pindex], struct page, pcp_list);		\
										\
	if (page) {								\
		KUNIT_FAIL(test, "PCPlist %d on CPU %d wasn't empty", i, cpu); \
		dump_page(page, "unexpectely on pcplist");			\
	}									\
})

static void action_drain_pages_all(void *unused)
{
	int cpu;
	for_each_online_cpu(cpu)
		drain_pages(cpu);
}

/* Run before each test.*/
static int test_init(struct kunit *test)
{
	struct zone *zone_normal;
	int cpu;

	if (isolated_node == NUMA_NO_NODE)
		kunit_skip(test, "No fake NUMA node ID allocated");

	zone_normal = &NODE_DATA(isolated_node)->node_zones[ZONE_NORMAL];

	/*
	* Nothing except these tests should be allocating from the fake node so
	* the pcplists should be empty. Obviously this is racy but at least it can
	* probabilistically detect issues that would otherwise make for really 
	* confusing test results.
	*/
	for_each_possible_cpu(cpu) {
		struct per_cpu_pages *pcp = per_cpu_ptr(zone_normal->per_cpu_pageset, cpu);
		unsigned long flags;
		int i;

		spin_lock_irqsave(&pcp->lock, flags);
		for (i = 0; i < ARRAY_SIZE(pcp->lists); i++)
			EXPECT_PCPLIST_EMPTY(test, zone_normal, cpu, i);
		spin_unlock_irqrestore(&pcp->lock, flags);
	}

	/* Also ensure we don't leave a mess for the next test .*/
	kunit_add_action(test, action_drain_pages_all, NULL);

	return 0;
}

static int memory_block_online_cb(struct memory_block *mem, void *unused)
{
	return memory_block_online(mem);
}

struct region {
	int node;
	unsigned long start;
	unsigned long size;
};

/*
* Unplug some memory from a "real" node and plug it into the isolated node, for use
* during the tests.
*/
static int populate_isolated_node(struct kunit_suite *suite)
{
	struct zone *zone_movable = &NODE_DATA(0)->node_zones[ZONE_MOVABLE];
	phys_addr_t zone_start = zone_movable->zone_start_pfn << PAGE_SHIFT; // Address in byte
	phys_addr_t zone_size = zone_movable->spanned_pages << PAGE_SHIFT;
	unsigned long bs = memory_block_size_bytes();
	u64 start = round_up(zone_start, bs);
	/*Plug a memory block if we can find it.*/
	/* This round_down may give a 0 in result */
	unsigned long size = round_down(min(zone_size, bs),bs);
	int err;

	if (!size) {
		pr_err("Couldn't find ZONE_MOVABLE block to offline\n");
		pr_err("Try setting/expanding movablecore=\n");
		return -1;
	}

	err = offline_and_remove_memory(start, size);
	if (err) {
		pr_notice("Couldn't offline PFN 0x%llx - 0x%llx\n",
			start >> PAGE_SHIFT, (start+size) >> PAGE_SHIFT);
		return err;
	}
	err = add_memory(isolated_node, start, size, MMOP_ONLINE);
	if (err) {
		pr_notice("Couldn't add PFN 0x%llx - 0x%llx\n",
			start >> PAGE_SHIFT, (start + size) >> PAGE_SHIFT);
		goto add_and_online_memory;
	}

	/* Walk through each added block and online them */
	err = walk_memory_blocks(start, size, NULL, memory_block_online_cb);
	if (err){
		pr_notice("Couldn't online PFN 0x%llx - 0x%llx\n",
			start >> PAGE_SHIFT, (start+size) >> PAGE_SHIFT);
		goto remove_memory;
	}
	return 0;
remove_memory:
	if (WARN_ON(remove_memory(start, size)))
		return err;
add_and_online_memory:
	if (WARN_ON(add_memory(0, start, size, MMOP_ONLINE)))
		return  err;
	WARN_ON(walk_memory_blocks(start, size, NULL, memory_block_online_cb));
	return err;
}

static void depopulate_isolated_node(struct kunit_suite *suite)
{
	unsigned long start, size = memory_block_size_bytes();

	if (suite->suite_init_err)
		return;

	start = NODE_DATA(isolated_node)->node_start_pfn << PAGE_SHIFT;

	WARN_ON(remove_memory(start, size));
	WARN_ON(add_memory(0, start, size, MMOP_ONLINE));
	WARN_ON(walk_memory_blocks(start, size, NULL, memory_block_online_cb));
}

static struct kunit_case test_cases[] = {{}};

static struct kunit_suite page_alloc_test_suite = {
	.name = "page_alloc",
	.test_cases = test_cases,
	.suite_init= populate_isolated_node,
	.suite_exit = depopulate_isolated_node,
	.init = test_init,
};

kunit_test_suite(page_alloc_test_suite);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
