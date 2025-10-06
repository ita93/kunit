# Quick Reference: Syzkaller Bug Tests

## What the Syzkaller Reproducer Does

```
1. mmap() block device          → Map 11MB of memory
2. madvise(MADV_HWPOISON)       → Poison 8MB (simulate bad RAM)
3. process_madvise(MADV_COLD)   → Try to mark pages as cold → 💥 CRASH!
```

**Why it crashes:** `madvise_cold_or_pageout_pte_range()` doesn't check `PageHWPoison` before manipulating pages.

## The Fix (1 line change in mm/madvise.c)

```c
folio = page_folio(page);

+ if (PageHWPoison(page)) {
+     folio_put(folio);
+     continue;
+ }

folio_clear_referenced(folio);
// ... rest of processing
```

## Tests Added to mm/page_alloc_test.c

| Test | What it does |
|------|--------------|
| `test_madvise_cold_hwpoison` | Basic poison flag test |
| `test_memory_failure_on_page` | Real `memory_failure()` API test |
| `test_alloc_avoids_poisoned_pages` | Verify allocator skips poisoned pages |

## Quick Commands

```bash
# Build with KUnit
make defconfig
./scripts/config -e KUNIT -e MEMORY_FAILURE
make -j$(nproc)

# Run tests
./tools/testing/kunit/kunit.py run page_alloc

# Run specific test
./tools/testing/kunit/kunit.py run page_alloc.test_madvise_cold_hwpoison
```

## Key Functions

| Function | Purpose |
|----------|---------|
| `SetPageHWPoison(page)` | Mark page as poisoned |
| `PageHWPoison(page)` | Check if page is poisoned |
| `ClearPageHWPoison(page)` | Clear poison flag |
| `memory_failure(pfn, flags)` | Official poison API |
| `unpoison_memory(pfn)` | Unpoison a page |

## Important Flags

- `MF_SW_SIMULATED` - Software-simulated failure (for testing)
- `MADV_HWPOISON` (0xe) - madvise flag to poison pages
- `MADV_COLD` (0x65) - madvise flag to mark pages cold

## Config Requirements

```
CONFIG_KUNIT=y
CONFIG_MEMORY_FAILURE=y
CONFIG_MEMORY_HOTPLUG=y
CONFIG_NUMA=y
```

## File Locations

- **Bug location:** `mm/madvise.c:madvise_cold_or_pageout_pte_range()`
- **Tests:** `mm/page_alloc_test.c`
- **Poison API:** `mm/memory-failure.c`
- **Page flags:** `include/linux/page-flags.h`
