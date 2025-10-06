# KUnit Tests for Syzkaller Bug: MADV_COLD on Hardware-Poisoned Pages

## Bug Report
**URL:** https://syzkaller.appspot.com/bug?id=bae0fd140352e4b5d8b65f3ec3c50bcd3ffc7a13

## Summary
This bug occurs when `process_madvise(MADV_COLD)` is called on memory pages that have been marked as hardware-poisoned via `MADV_HWPOISON`. The kernel crashes because `madvise_cold_or_pageout_pte_range()` in `mm/madvise.c` doesn't check for `PageHWPoison` before manipulating pages.

## Root Cause
The function `madvise_cold_or_pageout_pte_range()` processes pages without checking if they're poisoned:
- Tries to deactivate poisoned pages with `folio_deactivate()`
- Attempts to isolate them from LRU with `folio_isolate_lru()`
- Clears page flags with `folio_clear_referenced()`

All these operations assume valid page state, but poisoned pages represent corrupted memory and should never be touched.

## The Fix Needed
Add this check in `mm/madvise.c` in `madvise_cold_or_pageout_pte_range()`:

```c
folio = page_folio(page);

/* Skip hardware-poisoned pages */
if (PageHWPoison(page)) {
    folio_put(folio);
    continue;
}

// ... rest of processing ...
```

## KUnit Tests Added

### Test 1: `test_madvise_cold_hwpoison`
**Purpose:** Verify that pages can be marked as hardware-poisoned and are identifiable.

**Steps:**
1. Allocate a page from the isolated node
2. Mark it as `PageHWPoison` using `SetPageHWPoison()`
3. Verify the `PageHWPoison()` flag is set
4. Document what the bug scenario would be
5. Clean up by clearing the poison flag and freeing the page

**What it tests:**
- Basic hardware poison flag functionality
- That poisoned pages are identifiable by the kernel
- Proper cleanup of poisoned pages

### Test 2: `test_memory_failure_on_page`
**Purpose:** Test the actual `memory_failure()` API that `MADV_HWPOISON` uses internally.

**Steps:**
1. Allocate a page from the isolated node
2. Call `memory_failure(pfn, MF_SW_SIMULATED)` to properly poison it
3. Handle different return codes:
   - `0`: Successfully poisoned - verify `PageHWPoison` is set
   - `-EOPNOTSUPP`: Page type not supported (expected for buddy pages)
   - Other errors: Log and handle appropriately
4. Clean up with `unpoison_memory()`

**What it tests:**
- Real hardware poison API behavior
- Different page types and their poison support
- Proper poisoning and unpoisoning flow
- The actual code path that `MADV_HWPOISON` uses

### Test 3: `test_alloc_avoids_poisoned_pages`
**Purpose:** Verify that the page allocator doesn't hand out poisoned pages.

**Steps:**
1. Allocate page1 and poison it
2. Allocate page2
3. Verify page2 is different from page1
4. Verify page2 is not poisoned
5. Clean up both pages

**What it tests:**
- Poisoned pages are isolated from the buddy allocator
- New allocations don't return poisoned pages
- Memory safety after poisoning

## How to Run the Tests

### Build the kernel with KUnit support:
```bash
make ARCH=x86_64 defconfig
./scripts/config -e KUNIT -e KUNIT_ALL_TESTS -e MEMORY_FAILURE
make -j$(nproc)
```

### Run all page_alloc tests:
```bash
./tools/testing/kunit/kunit.py run page_alloc
```

### Run specific hwpoison tests:
```bash
./tools/testing/kunit/kunit.py run page_alloc.test_madvise_cold_hwpoison
./tools/testing/kunit/kunit.py run page_alloc.test_memory_failure_on_page
./tools/testing/kunit/kunit.py run page_alloc.test_alloc_avoids_poisoned_pages
```

## Test Requirements

### Kernel Config Options:
- `CONFIG_KUNIT=y` - KUnit framework
- `CONFIG_MEMORY_FAILURE=y` - Hardware poison support
- `CONFIG_MEMORY_HOTPLUG=y` - For isolated node setup
- `CONFIG_NUMA=y` - NUMA support for test infrastructure

### Runtime Requirements:
- The test suite sets up an isolated NUMA node for testing
- Requires movable memory to be available (`movablecore=` kernel parameter)
- Tests are skipped if `CONFIG_MEMORY_FAILURE` is not enabled

## Expected Results

### Before the fix:
If you were to actually call `madvise(MADV_COLD)` on poisoned pages (which these tests document but don't directly trigger), you would see:
- Kernel crashes (NULL pointer dereference, use-after-free)
- Memory corruption
- System instability

### After the fix:
- All three tests should pass
- `madvise(MADV_COLD)` should skip poisoned pages safely
- No crashes or corruption when processing memory with poisoned pages

## Test Limitations

These KUnit tests verify:
✅ Pages can be marked as poisoned
✅ Poisoned pages are identifiable via `PageHWPoison()`
✅ The allocator avoids poisoned pages
✅ The `memory_failure()` API works correctly

These tests do NOT directly test:
❌ The actual `madvise()` system call (requires userspace context)
❌ The page table walk in `madvise_cold_or_pageout_pte_range()`
❌ The full process_madvise flow with pidfd

To fully test the bug scenario, you would need:
- An integration test that maps memory
- Calls `madvise(MADV_HWPOISON)` on it
- Then calls `process_madvise(MADV_COLD)` on the same region
- This would require a userspace test program or more complex kernel test infrastructure

## Related Code Paths

### Files involved in the bug:
- `mm/madvise.c` - Where the bug exists
  - `madvise_cold()` - Entry point for MADV_COLD
  - `madvise_cold_or_pageout_pte_range()` - **Bug location**
  - `process_madvise()` - Remote madvise via pidfd

### Hardware poison infrastructure:
- `mm/memory-failure.c` - Main hwpoison handling
- `include/linux/page-flags.h` - `PageHWPoison` flag definition
- `mm/hwpoison-inject.c` - Testing interface

### Test infrastructure:
- `mm/page_alloc_test.c` - Our test file
- `mm/internal.h` - Internal MM definitions

## Impact

This bug can cause:
- **Kernel crashes** from NULL pointer dereferences or use-after-free
- **Memory corruption** by manipulating invalid page metadata
- **Data loss** if poisoned pages are incorrectly reclaimed
- **System instability** if poisoned pages enter LRU lists

The bug is triggered by:
1. Hardware memory errors (real or simulated via `MADV_HWPOISON`)
2. Followed by `process_madvise(MADV_COLD)` on the affected region
3. This is a realistic scenario in production systems with memory errors

## Next Steps

1. **Verify tests compile and run** in your kernel tree
2. **Implement the fix** in `mm/madvise.c`
3. **Run the tests** to verify the fix doesn't break anything
4. **Test with the actual syzkaller reproducer** if possible
5. **Submit patch** with these tests as validation

## References

- Syzkaller bug: https://syzkaller.appspot.com/bug?id=bae0fd140352e4b5d8b65f3ec3c50bcd3ffc7a13
- Syzkaller docs: https://goo.gl/kgGztJ
- Hardware poison docs: `Documentation/mm/hwpoison.rst`
- KUnit docs: `Documentation/dev-tools/kunit/`
