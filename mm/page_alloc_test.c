#include "linux/mm.h"
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

// PN: static function clone from other files
static inline enum zone_type gfp_zone_cloned(gfp_t flags)
{
	enum zone_type z;
	int bit = (__force int)(flags & GFP_ZONEMASK);

	z = (GFP_ZONE_TABLE >> (bit * GFP_ZONES_SHIFT)) &
	    ((1 << GFP_ZONES_SHIFT) - 1);
	VM_BUG_ON((GFP_ZONE_BAD >> bit) & 1);
	return z;
}

// PN: We do not want any percpu list
#define EXPECT_PCPLIST_EMPTY(test, zone, cpu, pindex)                         \
	({                                                                    \
		struct per_cpu_pages *pcp =                                   \
			per_cpu_ptr(zone->per_cpu_pageset, cpu);              \
		struct page *page;                                            \
                                                                              \
		lockdep_assert_held(&pcp->lock);                              \
		page = list_first_entry_or_null(&pcp->lists[pindex],          \
						struct page, pcp_list);       \
                                                                              \
		if (page) {                                                   \
			KUNIT_FAIL(test, "PCPlist %d on CPU %d wasn't empty", \
				   i, cpu);                                   \
			dump_page(page, "unexpectely on pcplist");            \
		}                                                             \
	})

// PN: Assert if page is not belong to specified zone
// Basically, it check if the PFN is in bound
#define EXPECT_WITHIN_ZONE(test, page, zone)                                   \
	({                                                                     \
		unsigned long pfn = page_to_pfn(page);                         \
		unsigned long start_pfn = zone->zone_start_pfn;                \
		unsigned long end_pfn = start_pfn + zone->spanned_pages;       \
                                                                               \
		KUNIT_EXPECT_TRUE_MSG(test, pfn >= start_pfn && pfn < end_pfn, \
				      "Wanted PFN 0x%lx - 0x%lx, got 0x%lx",   \
				      start_pfn, end_pfn, pfn);                \
		KUNIT_EXPECT_PTR_EQ_MSG(test, page_zone(page), zone,           \
					"Wanted %px (%s), got %px (%s)", zone, \
					zone->name, page_zone(page),           \
					page_zone(page)->name);                \
	})

static void action_nodemask_free(void *ctx)
{
	NODEMASK_FREE(ctx);
}

/*
 * Call __alloc_pages_noprof with a nodemask containing only the nid.
 * never return NULL
 */
static inline struct page *alloc_pages_force_nid(struct kunit *test, gfp_t gfp,
						 int order, int nid)
{
	// this will alloc an object name nodemask
	// This equavalent to:
	// nodemask_t _nodemask, *nodemask = &_nodemask
	NODEMASK_ALLOC(nodemask_t, nodemask, GFP_KERNEL);
	struct page *page;
	KUNIT_ASSERT_NOT_NULL(test, nodemask);
	kunit_add_action(test, action_nodemask_free, &nodemask);
	// unselect all NUMA node
	nodes_clear(*nodemask);
	// select ony the our node (nid: the input parameter)
	node_set(nid, *nodemask);

	// Alloc a page from node nid
	page = __alloc_pages_noprof(GFP_KERNEL, 0, nid, nodemask);
	KUNIT_ASSERT_NOT_NULL(test, page);
	// let compare the NID
	// nid is stored in the page flags
	int real_nid = page_to_nid(page);
	KUNIT_ASSERT_EQ(test, real_nid, nid);
	return page;
}

// PN: head is the input buddy freelist head
static inline bool page_on_buddy_list(struct page *want_page,
				      struct list_head *head)
{
	struct page *found_page;

	list_for_each_entry(found_page, head, buddy_list) {
		if (found_page == want_page)
			return true;
	}

	return false;
}

/* Test case parameters that are independent of alloc order. */
static const struct {
	gfp_t gfp_flags;
	enum zone_type want_zone;
} alloc_fresh_gfps[] = {
	/*
	 * The way we currently set up the isolated node, everything ends up in
	 * ZONE_NORMAL.
	 * PN
	 * Remember that the zone mask is #define GFP_ZONEMASK	(__GFP_DMA|__GFP_HIGHMEM|__GFP_DMA32|__GFP_MOVABLE)
	 * That mean only GFP_DMA32 has a zone selection bit.
	 * static inline enum zone_type gfp_zone(gfp_t flags) should return ZONE_NORMAL for all
	 * test cases, in other words, the highest zone index is ZONE_NORMAL
	 * Next, because we has NUMA enable, the ac->zonelist (in prepare_alloc_nodes) is 
	 * NODE_DATA(nid)->node_zonelists
	 */
	// __GFP_RECLAIM| __GFP_IO |__GFP_FS
	{ .gfp_flags = GFP_KERNEL, .want_zone = ZONE_NORMAL },
	// __GFP_HIGH | __GFP_KSWAPD_RECLAIM
	{ .gfp_flags = GFP_ATOMIC, .want_zone = ZONE_NORMAL },
	// __GFP_RECLAIM| __GFP_IO | __GFP_FS|__GFP_HARDWALL
	{ .gfp_flags = GFP_USER, .want_zone = ZONE_NORMAL },
	// GFP_DMA32
	{ .gfp_flags = GFP_DMA32, .want_zone = ZONE_NORMAL },
};

struct alloc_fresh_test_case {
	int order;
	int gfp_idx;
};

/* Generate test cases as the cross product of orders and alloc_fresh_gfps. */
static const void *alloc_fresh_gen_params(const void *prev, char *desc)
{
	/* Buffer to avoid allocations */
	static struct alloc_fresh_test_case tc;

	// First run, init order and gfp_idx to 0
	if (!prev) {
		/* First call */
		tc.order = 0;
		tc.gfp_idx = 0;
		return &tc;
	}

	// for each run, increase the gfp_index
	tc.gfp_idx++;
	if (tc.gfp_idx >= ARRAY_SIZE(alloc_fresh_gfps)) {
		// we already generate test for all gfps of this order
		// reinit gfp_idx to 0, and increase the order, it will
		// starting to generate data for the next oder
		tc.gfp_idx = 0;
		tc.order++;
	}

	// all order was generated, return NIL to terminate
	if (tc.order > MAX_PAGE_ORDER)
		/*Finished*/
		return NULL;
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "order %d %pGg\n", tc.order,
		 &alloc_fresh_gfps[tc.gfp_idx].gfp_flags);
	return &tc;
}

/* Smoke test: allocate from a node where everything is in a pristine state. */
// this is a iteration of the outer (implicit) loop:
// for (gfp_idx = 0; gfp_idx < len(alloc_fresh_gfps); gfp_idx++) {
//	for (order = 0; order <= MAX_PAGE_ORDER; order++) {
static void test_alloc_fresh(struct kunit *test)
{
	const struct alloc_fresh_test_case *tc = test->param_value;
	// These two are the parameters for the test cases, not related to the kernel code
	gfp_t gfp_flags = alloc_fresh_gfps[tc->gfp_idx].gfp_flags;
	enum zone_type want_zone_type = alloc_fresh_gfps[tc->gfp_idx].want_zone;
	// Get the zone descriptor of want_zone_type zone
	// each zone type is store in zone_nodes array element
	// actually we always want zone normal
	struct zone *want_zone =
		&NODE_DATA(isolated_node)->node_zones[want_zone_type];
	struct list_head *buddy_list;
	struct per_cpu_pages *pcp;
	struct page *page, *merged_page;
	int cpu, migrate_type;

	// PN: new definitions
	// batch is always 7, count can change
	cpu = get_cpu();
	pcp = per_cpu_ptr(want_zone->per_cpu_pageset, cpu);
	// MIGRATE_PCPTYPES * (PAGE_ALLOC_COSTLY_ORDER + 1) = 3 * (3 + 1) = 12
	KUNIT_EXPECT_EQ(test, ARRAY_SIZE(pcp->lists), 12);
	if (MIGRATE_PCPTYPES * tc->order + MIGRATE_UNMOVABLE <
	    ARRAY_SIZE(pcp->lists)) {
		buddy_list = &pcp->lists[MIGRATE_PCPTYPES * tc->order +
					 MIGRATE_UNMOVABLE];
		// I think the buddy list should be empty
		KUNIT_EXPECT_TRUE(test, list_empty(buddy_list));
	}
	KUNIT_EXPECT_EQ(test, pcp->count, 0);
	put_cpu();

	// PN: inside the allocator
	// The migrate type should be 0 (UNMOVABLE)
#define GFP_MOVABLE_MASK (__GFP_RECLAIMABLE | __GFP_MOVABLE)
#define GFP_MOVABLE_SHIFT 3
	migrate_type = (gfp_flags & GFP_MOVABLE_MASK) >> GFP_MOVABLE_SHIFT;
	KUNIT_EXPECT_EQ(test, migrate_type, 0);
#undef GFP_MOVABLE_MASK
#undef GFP_MOVABLE_SHIFT
	// PN: Verify the output zonetype (raw check)
	NODEMASK_ALLOC(nodemask_t, nodemask, GFP_KERNEL);
	nodes_clear(*nodemask);
	node_set(isolated_node, *nodemask);
	struct zoneref *ref = __next_zones_zonelist(
		NODE_DATA(isolated_node)->node_zonelists[0]._zonerefs,
		gfp_zone_cloned(gfp_flags), nodemask);
	// the first zone returned should be ZONE_NORMAL, ZONE_DMA or ZONE_DMA32
	KUNIT_EXPECT_TRUE(test, ref->zone_idx <= gfp_zone_cloned(gfp_flags));

	page = alloc_pages_force_nid(test, gfp_flags, tc->order, isolated_node);

	EXPECT_WITHIN_ZONE(test, page, want_zone);

	cpu = get_cpu();
	// PN: Try accessing pcp details
	// I feel it is wierd because even when the order is costly, the pcp count still change
	// as I understand the count should not be changed because rmqueue_bulk will not be called.
	pcp = per_cpu_ptr(want_zone->per_cpu_pageset, cpu);
	// why is it 6?
	int pcp_count_before_free = pcp->count;
	__free_pages(
		page,
		0); // free only one page, otherwise, the drain_zone_pages() will get issue
	KUNIT_EXPECT_EQ(
		test, pcp->count,
		pcp_count_before_free +
			1); //the pcp is a pointer, so don't need to reset it
	put_cpu();

	/*
	 * Should end up back in the free are when drained. Because everything is free,
	 * it should get buddy-merged up to the maximum order.
	 */
	drain_zone_pages(want_zone, pcp);
	// check pcp after drain page
	cpu = get_cpu();
	pcp = per_cpu_ptr(want_zone->per_cpu_pageset, cpu);
	KUNIT_EXPECT_EQ(test, pcp->count, 0);
	put_cpu();
	// End of pcp
	KUNIT_EXPECT_TRUE(test, PageBuddy(page));
	KUNIT_EXPECT_EQ(test, buddy_order(page), MAX_PAGE_ORDER);
	KUNIT_EXPECT_TRUE(test, list_empty(&pcp->lists[MIGRATE_UNMOVABLE]));
	merged_page =
		pfn_to_page(round_down(page_to_pfn(page), 1 << MAX_PAGE_ORDER));
	buddy_list = &want_zone->free_area[MAX_PAGE_ORDER]
			      .free_list[MIGRATE_UNMOVABLE];
	KUNIT_EXPECT_TRUE(test, page_on_buddy_list(merged_page, buddy_list));
}

static void test_buddy_merge_on_free(struct kunit *test)
{
	int order = 2; // Test with order-2 (4 contiguous pages)
	// allocation should still endup in ZONE_MOVABLE
	struct page *page, *realloc_page;
	int first_pfn, second_pfn;
	struct zone* zone_normal = &NODE_DATA(isolated_node)->node_zones[ZONE_NORMAL];

	// Allocate a block of order-2
	page = alloc_pages_force_nid(test, GFP_KERNEL, order, isolated_node);
	KUNIT_ASSERT_NOT_NULL(test, page);
	first_pfn = page_to_pfn(page);
	KUNIT_ASSERT_EQ(test, first_pfn, zone_normal->zone_start_pfn);
	KUNIT_ASSERT_STREQ(test, page_zone(page)->name, "Normal");

	// Free the block
	__free_pages(page, 0);
	// Drain per-cpu pages to force merge into buddy system
	drain_all_pages(zone_normal);

	// Try to allocate again; should get the same page back
	realloc_page =
		alloc_pages_force_nid(test, GFP_KERNEL, order, isolated_node);
	KUNIT_ASSERT_NOT_NULL(test, realloc_page);
	second_pfn = page_to_pfn(realloc_page);
	KUNIT_ASSERT_EQ(test, second_pfn, first_pfn);

	// Clean up
	__free_pages(realloc_page, order);
	// We free allpage, hence cannot drain
}

// This test tries to verify the state of zone free area after hot plug.
// We don't need to do any allocation in this test
static void test_zone_buddy_list_after_hotplug(struct kunit *test)
{
	// How was memory plugged
	// populate_isolated_node()
	//  -> add_memory(isolated_node, start, size, MMOP_ONLINE);
	//    -> __add_memory(isolated_node, start, size, MMOP_ONLINE);
	//      -> arch_add_memory()
	//        -> add_pages()
	//          -> __add_pages()
	//            -> sparse_add_section()
	//              -> set_section_nid()
	//  -> walk_memory_blocks(start, size, NULL, memory_block_online_cb);
	//     -> online_pages()
	//       -> online_pages_range()
	//         -> (*online_page_callback)(page, order)
	//           -> generic_online_page()
	//             -> __free_pages_core()
	//               -> __free_pages_core_init()
	//                 -> __free_pages_core_init_nid()
	//                   -> __free_pages_core_init_nid_init()
}

// Drain pcplist pages
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
		struct per_cpu_pages *pcp =
			per_cpu_ptr(zone_normal->per_cpu_pageset, cpu);
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
	phys_addr_t zone_start = zone_movable->zone_start_pfn
				 << PAGE_SHIFT; // Address in byte
	phys_addr_t zone_size = zone_movable->spanned_pages << PAGE_SHIFT;
	// memory block size is 0 if there is no movable page
	unsigned long bs = memory_block_size_bytes();
	u64 start = round_up(zone_start, bs);
	/*Plug a memory block if we can find it.*/
	/* This round_down may give a 0 in result */
	unsigned long size = round_down(min(zone_size, bs), bs);
	int err;

	if (!size) {
		pr_err("Couldn't find ZONE_MOVABLE block to offline\n");
		pr_err("Try setting/expanding movablecore=\n");
		return -1;
	}

	// removing memory from a real node
	err = offline_and_remove_memory(start, size);
	if (err) {
		pr_notice("Couldn't offline PFN 0x%llx - 0x%llx\n",
			  start >> PAGE_SHIFT, (start + size) >> PAGE_SHIFT);
		return err;
	}

	// and plug it to the isolated node
	err = add_memory(isolated_node, start, size, MMOP_ONLINE);
	if (err) {
		pr_notice("Couldn't add PFN 0x%llx - 0x%llx\n",
			  start >> PAGE_SHIFT, (start + size) >> PAGE_SHIFT);
		goto add_and_online_memory;
	}

	/* Walk through each added block and online them */
	err = walk_memory_blocks(start, size, NULL, memory_block_online_cb);
	if (err) {
		pr_notice("Couldn't online PFN 0x%llx - 0x%llx\n",
			  start >> PAGE_SHIFT, (start + size) >> PAGE_SHIFT);
		goto remove_memory;
	}
	return 0;
remove_memory:
	if (WARN_ON(remove_memory(start, size)))
		return err;
add_and_online_memory:
	if (WARN_ON(add_memory(0, start, size, MMOP_ONLINE)))
		return err;
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

static struct kunit_case test_cases[] = {
	KUNIT_CASE_PARAM(test_alloc_fresh, alloc_fresh_gen_params),
	KUNIT_CASE(test_buddy_merge_on_free),
	{}
};

static struct kunit_suite page_alloc_test_suite = {
	.name = "page_alloc",
	.test_cases = test_cases,
	// Adding memory to the isolated node and online thme
	.suite_init = populate_isolated_node,
	// Remove the memory from isolated node, add them back to the node 0
	// and online them again
	.suite_exit = depopulate_isolated_node,
	.init = test_init,
};

kunit_test_suite(page_alloc_test_suite);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
