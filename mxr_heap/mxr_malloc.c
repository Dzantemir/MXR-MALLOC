#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#else
#include "sdkconfig.h"
#endif

#include "esp_attr.h"

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

/*
 * Placement attributes.
 *
 * MXR_IRAM_ATTR        : core malloc/free hot path.
 * MXR_IRAM_INLINE_ATTR : small inline helpers used by the hot path.
 * MXR_IRAM_ALLOC_ATTR  : allocation family (calloc/zalloc/realloc) and
 *                        their word helpers. Only placed in IRAM when
 *                        CONFIG_MXR_IRAM_PATH_ALLOC_FAMILY is enabled.
 */
#ifdef CONFIG_MXR_IRAM_HOT_PATH_DISABLED
#define MXR_IRAM_ATTR
#define MXR_IRAM_INLINE_ATTR
#define MXR_IRAM_ALLOC_ATTR
#else
#define MXR_IRAM_ATTR IRAM_ATTR
#define MXR_IRAM_INLINE_ATTR IRAM_ATTR
#ifdef CONFIG_MXR_IRAM_PATH_ALLOC_FAMILY
#define MXR_IRAM_ALLOC_ATTR IRAM_ATTR
#else
#define MXR_IRAM_ALLOC_ATTR
#endif
#endif

#include "mxr_malloc.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "mxr_malloc";

#ifndef CONFIG_MXR_REGION_PERCENTS
#define CONFIG_MXR_REGION_PERCENTS "15,45,20"
#endif

#ifndef CONFIG_MXR_REGION_SIZES
#define CONFIG_MXR_REGION_SIZES "4,132,1024"
#endif

#ifndef CONFIG_MXR_IRAM_RESERVE_BYTES
#define CONFIG_MXR_IRAM_RESERVE_BYTES 2048
#endif

#ifndef CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES
#define CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES 0
#endif

extern void vPortETSIntrLock(void);
extern void vPortETSIntrUnlock(void);

static inline void MXR_IRAM_INLINE_ATTR mxr_lock(void)
{
    vPortETSIntrLock();
}

static inline void MXR_IRAM_INLINE_ATTR mxr_unlock(void)
{
    vPortETSIntrUnlock();
}

/*
 * Allocator state.
 */
static mxr_desc_t s_desc[CONFIG_MXR_MAX_DESC];
static uint16_t s_desc_count;
static uint8_t s_region_count;
static uint8_t *s_arena_base;
static uint16_t s_arena_total_units;
static bool s_initialized;
static mxr_status_t s_stats;

#ifdef CONFIG_MXR_USE_IRAM
static bool s_iram_enabled;
static uint8_t *s_iram_base;
static uint16_t s_iram_total_units;
static uint16_t s_iram_free_units;
static uint16_t s_iram_min_free_units;
static uint32_t s_iram_exec_allocs;
static uint32_t s_iram_fallback_allocs;
#endif

/*
 * Active region count.
 *
 * Every preset (including Custom) drives regions through the
 * Kconfig parser, so the count is always CONFIG_MXR_REGIONS.
 */
#define MXR_ACTIVE_TOTAL_REGIONS CONFIG_MXR_REGIONS

static mxr_region_t s_region[MXR_ACTIVE_TOTAL_REGIONS];

static inline void MXR_IRAM_ALLOC_ATTR mxr_memset_words(void *ptr, uint16_t units)
{
    uint32_t *p = (uint32_t *)ptr;
    for (uint16_t i = 0; i < units; i++)
    {
        p[i] = 0;
    }
}

static inline void MXR_IRAM_ALLOC_ATTR mxr_memcpy_words(void *dst, const void *src, uint16_t units)
{
    uint32_t *d = (uint32_t *)dst;
    const uint32_t *s = (const uint32_t *)src;
    for (uint16_t i = 0; i < units; i++)
    {
        d[i] = s[i];
    }
}

/* ================================================================
 *  Bitmap search (DRAM only)
 * ================================================================ */

#ifdef CONFIG_MXR_SEARCH_BITMAP

/*
 * Bitmap is carved from the end of the arena at init time.
 * Size is proportional to actual arena, not max 128 KB.
 *
 * For 80 KB DRAM:
 *   80000 / 4 = 20000 units
 *   20000 / 8 = 2500 bytes
 */
static uint32_t *s_bitmap;
static uint16_t s_bitmap_units;

static inline uint32_t MXR_IRAM_INLINE_ATTR mxr_mask_low_bits(uint16_t bits)
{
    if (bits >= 32)
    {
        return 0xFFFFFFFFu;
    }
    if (bits == 0)
    {
        return 0;
    }
    return (uint32_t)((1u << bits) - 1u);
}

static void MXR_IRAM_ATTR mxr_bitmap_set_range(uint16_t off_units, uint16_t len_units)
{
    if (len_units == 0)
    {
        return;
    }
    if (off_units >= s_arena_total_units)
    {
        return;
    }

    uint32_t end = (uint32_t)off_units + len_units;
    if (end > s_arena_total_units)
    {
        end = s_arena_total_units;
    }

    while (off_units < end)
    {
        uint16_t word_index = off_units >> 5;
        uint16_t bit = off_units & 31;
        uint16_t chunk = 32 - bit;
        uint16_t remain = (uint16_t)(end - off_units);
        if (chunk > remain)
        {
            chunk = remain;
        }
        uint32_t mask = mxr_mask_low_bits(chunk) << bit;
        s_bitmap[word_index] |= mask;
        off_units += chunk;
    }
}

static void MXR_IRAM_ATTR mxr_bitmap_clear_range(uint16_t off_units, uint16_t len_units)
{
    if (len_units == 0)
    {
        return;
    }
    if (off_units >= s_arena_total_units)
    {
        return;
    }

    uint32_t end = (uint32_t)off_units + len_units;
    if (end > s_arena_total_units)
    {
        end = s_arena_total_units;
    }

    while (off_units < end)
    {
        uint16_t word_index = off_units >> 5;
        uint16_t bit = off_units & 31;
        uint16_t chunk = 32 - bit;
        uint16_t remain = (uint16_t)(end - off_units);
        if (chunk > remain)
        {
            chunk = remain;
        }
        uint32_t mask = mxr_mask_low_bits(chunk) << bit;
        s_bitmap[word_index] &= ~mask;
        off_units += chunk;
    }
}

static int MXR_IRAM_ATTR mxr_bitmap_next_set(uint16_t start, uint16_t end_exclusive)
{
    if (end_exclusive > s_arena_total_units)
    {
        end_exclusive = s_arena_total_units;
    }
    if (start >= end_exclusive)
    {
        return -1;
    }

    uint16_t pos = start;
    while (pos < end_exclusive)
    {
        uint16_t word_index = pos >> 5;
        uint16_t word_start = word_index << 5;
        uint16_t bit = pos & 31;
        uint16_t high = (uint16_t)(end_exclusive - word_start);
        if (high > 32)
        {
            high = 32;
        }
        uint32_t mask = ~mxr_mask_low_bits(bit);
        mask &= mxr_mask_low_bits(high);
        uint32_t word = s_bitmap[word_index] & mask;
        if (word)
        {
            return (int)(word_start + __builtin_ctz(word));
        }
        pos = (uint16_t)(word_start + 32);
    }
    return -1;
}

static int MXR_IRAM_ATTR mxr_bitmap_next_clear(uint16_t start, uint16_t end_exclusive)
{
    if (end_exclusive > s_arena_total_units)
    {
        end_exclusive = s_arena_total_units;
    }
    if (start >= end_exclusive)
    {
        return -1;
    }

    uint16_t pos = start;
    while (pos < end_exclusive)
    {
        uint16_t word_index = pos >> 5;
        uint16_t word_start = word_index << 5;
        uint16_t bit = pos & 31;
        uint16_t high = (uint16_t)(end_exclusive - word_start);
        if (high > 32)
        {
            high = 32;
        }
        uint32_t mask = ~mxr_mask_low_bits(bit);
        mask &= mxr_mask_low_bits(high);
        uint32_t word = (~s_bitmap[word_index]) & mask;
        if (word)
        {
            return (int)(word_start + __builtin_ctz(word));
        }
        pos = (uint16_t)(word_start + 32);
    }
    return -1;
}

static bool MXR_IRAM_ATTR mxr_bitmap_find_from_start(
    int region_index,
    uint16_t units,
    uint16_t *out_off_units)
{
    uint16_t region_start = s_region[region_index].start_unit;
    uint16_t region_end = (uint16_t)(region_start + s_region[region_index].total_units);

    if (units == 0)
    {
        return false;
    }
    if ((uint32_t)region_end - region_start < units)
    {
        return false;
    }

    uint16_t cur = region_start;
    while ((uint32_t)cur + units <= region_end)
    {
        uint16_t window_end = (uint16_t)(cur + units);
        int allocated = mxr_bitmap_next_set(cur, window_end);
        if (allocated < 0)
        {
            *out_off_units = cur;
            return true;
        }
        int next_free = mxr_bitmap_next_clear((uint16_t)(allocated + 1), region_end);
        if (next_free < 0)
        {
            break;
        }
        cur = (uint16_t)next_free;
    }
    return false;
}

/*
 * Find largest contiguous free run in a region using bitmap.
 */
static uint16_t MXR_IRAM_ATTR mxr_bitmap_largest_free_run(int region_index)
{
    uint16_t region_start = s_region[region_index].start_unit;
    uint16_t region_end = (uint16_t)(region_start + s_region[region_index].total_units);

    if (region_end <= region_start)
    {
        return 0;
    }

    uint16_t largest = 0;
    uint16_t run_start = region_start;

    while (run_start < region_end)
    {
        /*
         * Find next allocated bit.
         */
        int alloc = mxr_bitmap_next_set(run_start, region_end);
        uint16_t run_end;
        if (alloc < 0)
        {
            run_end = region_end;
        }
        else
        {
            run_end = (uint16_t)alloc;
        }

        uint16_t gap = (uint16_t)(run_end - run_start);
        if (gap > largest)
        {
            largest = gap;
        }

        if (alloc < 0)
        {
            break;
        }

        /*
         * Skip to next free bit after allocated.
         */
        int next_free = mxr_bitmap_next_clear((uint16_t)(alloc + 1), region_end);
        if (next_free < 0)
        {
            break;
        }
        run_start = (uint16_t)next_free;
    }

    return largest;
}

#endif /* CONFIG_MXR_SEARCH_BITMAP */

/* ================================================================
 *  Basic conversions
 * ================================================================ */

static inline uint16_t MXR_IRAM_INLINE_ATTR mxr_bytes_to_units(size_t bytes)
{
    return (uint16_t)((bytes + MXR_UNIT_SIZE - 1) / MXR_UNIT_SIZE);
}

static inline void *MXR_IRAM_INLINE_ATTR mxr_units_to_ptr(uint16_t units)
{
    return (void *)(s_arena_base + (size_t)units * MXR_UNIT_SIZE);
}

static inline uint16_t MXR_IRAM_INLINE_ATTR mxr_ptr_to_units(const void *ptr)
{
    return (uint16_t)(((const uint8_t *)ptr - s_arena_base) / MXR_UNIT_SIZE);
}

#ifdef CONFIG_MXR_USE_IRAM
static inline void *MXR_IRAM_INLINE_ATTR mxr_iram_units_to_ptr(uint16_t units)
{
    return (void *)(s_iram_base + (size_t)units * MXR_UNIT_SIZE);
}

static inline uint16_t MXR_IRAM_INLINE_ATTR mxr_iram_ptr_to_units(const void *ptr)
{
    return (uint16_t)(((const uint8_t *)ptr - s_iram_base) / MXR_UNIT_SIZE);
}
#endif

typedef enum
{
    MXR_ARENA_NONE = 0,
    MXR_ARENA_DRAM,
    MXR_ARENA_IRAM,
} mxr_arena_id_t;

static mxr_arena_id_t MXR_IRAM_ATTR mxr_ptr_to_arena(const void *ptr)
{
    uintptr_t p = (uintptr_t)ptr;

    uintptr_t dram_start = (uintptr_t)s_arena_base;
    uintptr_t dram_end =
        dram_start + (size_t)s_arena_total_units * MXR_UNIT_SIZE;

    if (p >= dram_start && p < dram_end)
    {
        if ((p & (MXR_UNIT_SIZE - 1)) != 0)
        {
            return MXR_ARENA_NONE;
        }
        return MXR_ARENA_DRAM;
    }

#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        uintptr_t iram_start = (uintptr_t)s_iram_base;
        uintptr_t iram_end =
            iram_start + (size_t)s_iram_total_units * MXR_UNIT_SIZE;

        if (p >= iram_start && p < iram_end)
        {
            if ((p & (MXR_UNIT_SIZE - 1)) != 0)
            {
                return MXR_ARENA_NONE;
            }
            return MXR_ARENA_IRAM;
        }
    }
#endif

    return MXR_ARENA_NONE;
}

/* ================================================================
 *  Descriptor table
 *
 *  Sorted by off_flags.
 *  DRAM descriptors come first, IRAM descriptors last.
 * ================================================================ */

static void MXR_IRAM_ATTR mxr_desc_shift_right(uint16_t pos)
{
    for (uint16_t i = s_desc_count; i > pos; --i)
    {
        s_desc[i] = s_desc[i - 1];
    }
}

static void MXR_IRAM_ATTR mxr_desc_shift_left(int pos)
{
    for (int i = pos; i + 1 < (int)s_desc_count; ++i)
    {
        s_desc[i] = s_desc[i + 1];
    }
}

static int MXR_IRAM_ATTR mxr_desc_find_key(uint16_t key)
{
    int left = 0;
    int right = (int)s_desc_count;
    while (left < right)
    {
        int mid = (left + right) / 2;
        uint16_t cur = s_desc[mid].off_flags;
        if (cur == key)
        {
            return mid;
        }
        if (cur < key)
        {
            left = mid + 1;
        }
        else
        {
            right = mid;
        }
    }
    return -1;
}

#ifdef CONFIG_MXR_USE_IRAM
static int MXR_IRAM_ATTR mxr_desc_first_iram(void)
{
    int left = 0;
    int right = (int)s_desc_count;
    while (left < right)
    {
        int mid = (left + right) / 2;
        if (s_desc[mid].off_flags & MXR_OFF_FLAG_IRAM)
        {
            right = mid;
        }
        else
        {
            left = mid + 1;
        }
    }
    return left;
}
#endif

static bool MXR_IRAM_ATTR mxr_desc_insert_ex(
    uint16_t off_units,
    uint16_t len_units,
    bool iram,
    uint16_t len_flags)
{
    if (!s_initialized)
    {
        return false;
    }
    if (off_units > MXR_MAX_OFFSET_UNITS)
    {
        return false;
    }
    if (len_units == 0 || len_units > MXR_MAX_LEN_UNITS)
    {
        return false;
    }

    if (iram)
    {
#ifdef CONFIG_MXR_USE_IRAM
        if (!s_iram_enabled)
        {
            return false;
        }
        if ((uint32_t)off_units + len_units > s_iram_total_units)
        {
            return false;
        }
#else
        return false;
#endif
    }
    else
    {
        if ((uint32_t)off_units + len_units > s_arena_total_units)
        {
            return false;
        }
    }

    if (s_desc_count >= CONFIG_MXR_MAX_DESC)
    {
        s_stats.alloc_fail_table_full++;
        return false;
    }

    uint16_t key = mxr_desc_make_key(off_units, iram);

    /*
     * Binary search for insert position.
     */
    uint16_t pos;
    {
        int left = 0;
        int right = (int)s_desc_count;
        while (left < right)
        {
            int mid = (left + right) / 2;
            if (s_desc[mid].off_flags < key)
            {
                left = mid + 1;
            }
            else
            {
                right = mid;
            }
        }
        pos = (uint16_t)left;
    }

    if (pos < s_desc_count && s_desc[pos].off_flags == key)
    {
        return false;
    }

    /*
     * Overlap checks only inside same arena.
     */
    if (pos > 0 && mxr_desc_is_iram(&s_desc[pos - 1]) == iram)
    {
        uint16_t prev_off = mxr_desc_off(&s_desc[pos - 1]);
        uint16_t prev_len = mxr_desc_len(&s_desc[pos - 1]);
        uint32_t prev_end = (uint32_t)prev_off + prev_len;
        if (prev_end > off_units)
        {
            return false;
        }
    }

    if (pos < s_desc_count && mxr_desc_is_iram(&s_desc[pos]) == iram)
    {
        uint16_t next_off = mxr_desc_off(&s_desc[pos]);
        uint32_t new_end = (uint32_t)off_units + len_units;
        if (new_end > next_off)
        {
            return false;
        }
    }

    if (pos < s_desc_count)
    {
        mxr_desc_shift_right(pos);
    }

    mxr_desc_set(
        &s_desc[pos],
        off_units,
        len_units,
        iram ? MXR_OFF_FLAG_IRAM : 0,
        len_flags);

    s_desc_count++;
    s_stats.active_allocs = s_desc_count;
    if (s_stats.active_allocs > s_stats.max_active_allocs)
    {
        s_stats.max_active_allocs = s_stats.active_allocs;
    }

    return true;
}

static void MXR_IRAM_ATTR mxr_desc_remove(int index)
{
    if (index < 0 || index >= s_desc_count)
    {
        return;
    }
    if (index < (int)s_desc_count - 1)
    {
        mxr_desc_shift_left(index);
    }
    s_desc_count--;
    mxr_desc_clear(&s_desc[s_desc_count]);
    s_stats.active_allocs = s_desc_count;
}

static inline void MXR_IRAM_INLINE_ATTR mxr_desc_set_len(mxr_desc_t *d, uint16_t len_units)
{
    uint16_t flags = mxr_desc_len_flags(d);
    uint16_t stored_len;
    if (len_units == 0)
    {
        stored_len = 0;
    }
    else
    {
        stored_len = (uint16_t)(len_units - 1u);
    }
    d->len_flags = (uint16_t)((stored_len & MXR_LEN_MASK) | flags);
}

/* ================================================================
 *  DRAM region helpers
 * ================================================================ */

static int MXR_IRAM_ATTR mxr_region_by_off(uint16_t off_units)
{
    for (uint8_t i = 0; i < s_region_count; i++)
    {
        uint16_t start = s_region[i].start_unit;
        uint16_t end = (uint16_t)(start + s_region[i].total_units);
        if (off_units >= start && off_units < end)
        {
            return i;
        }
    }
    return -1;
}

static int MXR_IRAM_ATTR mxr_region_for_size(uint16_t len_units, uint32_t caps)
{
    for (uint8_t i = 0; i < s_region_count; i++)
    {
        if ((s_region[i].caps & caps) != caps)
        {
            continue;
        }
        if (len_units < s_region[i].min_units)
        {
            continue;
        }
        if (s_region[i].max_units != MXR_REGION_MAX_UNLIMITED)
        {
            if (len_units > s_region[i].max_units)
            {
                continue;
            }
        }
        return i;
    }
    return -1;
}

static bool MXR_IRAM_ATTR mxr_region_caps_ok(int region_index, uint32_t caps)
{
    if (region_index < 0 || region_index >= s_region_count)
    {
        return false;
    }
    return (s_region[region_index].caps & caps) == caps;
}

static bool MXR_IRAM_ATTR mxr_region_size_ok(int region_index, uint16_t units)
{
    if (region_index < 0 || region_index >= s_region_count)
    {
        return false;
    }
    if (units == 0)
    {
        return false;
    }
    if (units < s_region[region_index].min_units)
    {
        return false;
    }
    if (s_region[region_index].max_units != MXR_REGION_MAX_UNLIMITED)
    {
        if (units > s_region[region_index].max_units)
        {
            return false;
        }
    }
    return true;
}

static uint16_t MXR_IRAM_ATTR mxr_region_largest_free_units(uint8_t region_index)
{
    if (region_index >= s_region_count)
    {
        return 0;
    }

#ifdef CONFIG_MXR_SEARCH_BITMAP
    return mxr_bitmap_largest_free_run((int)region_index);
#else
    uint16_t region_start = s_region[region_index].start_unit;
    uint16_t region_end = (uint16_t)(region_start + s_region[region_index].total_units);

    uint16_t cur = region_start;
    uint16_t largest = 0;

    for (uint16_t i = 0; i < s_desc_count; i++)
    {
        if (mxr_desc_is_iram(&s_desc[i]))
        {
            break;
        }

        uint16_t off = mxr_desc_off(&s_desc[i]);
        uint16_t len = mxr_desc_len(&s_desc[i]);
        uint32_t block_end = (uint32_t)off + len;

        if (block_end <= region_start)
        {
            continue;
        }
        if (off >= region_end)
        {
            break;
        }

        if (off > cur)
        {
            uint16_t gap = (uint16_t)(off - cur);
            if (gap > largest)
            {
                largest = gap;
            }
        }

        if (block_end > cur)
        {
            cur = (uint16_t)block_end;
        }

        if (cur >= region_end)
        {
            break;
        }
    }

    if (region_end > cur)
    {
        uint16_t gap = (uint16_t)(region_end - cur);
        if (gap > largest)
        {
            largest = gap;
        }
    }

    return largest;
#endif
}

static void MXR_IRAM_ATTR mxr_region_allocated(int region_index, uint16_t units)
{
    if (region_index >= 0 && region_index < s_region_count)
    {
        if (s_region[region_index].free_units >= units)
        {
            s_region[region_index].free_units -= units;
        }
        else
        {
            s_region[region_index].free_units = 0;
        }

        if (s_region[region_index].free_units < s_region[region_index].min_free_units)
        {
            s_region[region_index].min_free_units = s_region[region_index].free_units;
        }
    }

    size_t bytes = (size_t)units * MXR_UNIT_SIZE;
    if (s_stats.free_bytes >= bytes)
    {
        s_stats.free_bytes -= bytes;
    }
    else
    {
        s_stats.free_bytes = 0;
    }

    if (s_stats.free_bytes < s_stats.min_free_bytes)
    {
        s_stats.min_free_bytes = s_stats.free_bytes;
    }
}

static void MXR_IRAM_ATTR mxr_region_released(int region_index, uint16_t units)
{
    if (region_index >= 0 && region_index < s_region_count)
    {
        uint32_t new_free =
            (uint32_t)s_region[region_index].free_units + units;
        if (new_free > s_region[region_index].total_units)
        {
            new_free = s_region[region_index].total_units;
        }
        s_region[region_index].free_units = (uint16_t)new_free;
    }

    s_stats.free_bytes += (size_t)units * MXR_UNIT_SIZE;
    if (s_stats.free_bytes > s_stats.total_bytes)
    {
        s_stats.free_bytes = s_stats.total_bytes;
    }
}

/* ================================================================
 *  DRAM free-block search
 * ================================================================ */

#ifndef CONFIG_MXR_SEARCH_BITMAP

static bool MXR_IRAM_ATTR mxr_find_free_from_start(
    int region_index,
    uint16_t units,
    uint16_t *out_off_units)
{
    uint16_t region_start = s_region[region_index].start_unit;
    uint16_t region_end = (uint16_t)(region_start + s_region[region_index].total_units);

    uint16_t cur = region_start;

    for (uint16_t i = 0; i < s_desc_count; i++)
    {
        if (mxr_desc_is_iram(&s_desc[i]))
        {
            break;
        }

        uint16_t off = mxr_desc_off(&s_desc[i]);
        uint16_t len = mxr_desc_len(&s_desc[i]);
        uint32_t block_end = (uint32_t)off + len;

        if (block_end <= region_start)
        {
            continue;
        }
        if (off >= region_end)
        {
            break;
        }

        if (off > cur)
        {
            uint16_t gap = (uint16_t)(off - cur);
            if (gap >= units)
            {
                *out_off_units = cur;
                return true;
            }
        }

        if (block_end > cur)
        {
            cur = (uint16_t)block_end;
        }

        if (cur >= region_end)
        {
            break;
        }
    }

    if (region_end > cur)
    {
        uint16_t gap = (uint16_t)(region_end - cur);
        if (gap >= units)
        {
            *out_off_units = cur;
            return true;
        }
    }

    return false;
}

#endif /* !CONFIG_MXR_SEARCH_BITMAP */

static bool MXR_IRAM_ATTR mxr_try_alloc_region(
    int region_index,
    uint16_t units,
    uint16_t *out_off_units)
{
    if (region_index < 0 || region_index >= s_region_count)
    {
        return false;
    }
    if (units == 0 || units > MXR_MAX_LEN_UNITS)
    {
        return false;
    }
    if (s_region[region_index].free_units < units)
    {
        return false;
    }

#ifdef CONFIG_MXR_SEARCH_BITMAP
    return mxr_bitmap_find_from_start(region_index, units, out_off_units);
#else
    return mxr_find_free_from_start(region_index, units, out_off_units);
#endif
}

/* ================================================================
 *  IRAM helpers
 * ================================================================ */

#ifdef CONFIG_MXR_USE_IRAM

static void MXR_IRAM_ATTR mxr_iram_allocated(uint16_t units)
{
    if (s_iram_free_units >= units)
    {
        s_iram_free_units -= units;
    }
    else
    {
        s_iram_free_units = 0;
    }

    if (s_iram_free_units < s_iram_min_free_units)
    {
        s_iram_min_free_units = s_iram_free_units;
    }

    size_t bytes = (size_t)units * MXR_UNIT_SIZE;
    if (s_stats.free_bytes >= bytes)
    {
        s_stats.free_bytes -= bytes;
    }
    else
    {
        s_stats.free_bytes = 0;
    }

    if (s_stats.free_bytes < s_stats.min_free_bytes)
    {
        s_stats.min_free_bytes = s_stats.free_bytes;
    }
}

static void MXR_IRAM_ATTR mxr_iram_released(uint16_t units)
{
    uint32_t new_free = (uint32_t)s_iram_free_units + units;
    if (new_free > s_iram_total_units)
    {
        new_free = s_iram_total_units;
    }
    s_iram_free_units = (uint16_t)new_free;

    s_stats.free_bytes += (size_t)units * MXR_UNIT_SIZE;
    if (s_stats.free_bytes > s_stats.total_bytes)
    {
        s_stats.free_bytes = s_stats.total_bytes;
    }
}

static bool MXR_IRAM_ATTR mxr_iram_find_free_from_start(
    uint16_t units,
    uint16_t *out_off_units)
{
    if (!s_iram_enabled)
    {
        return false;
    }
    if (units == 0 || units > s_iram_total_units)
    {
        return false;
    }
    if (s_iram_free_units < units)
    {
        return false;
    }

    uint16_t cur = 0;
    uint16_t end = s_iram_total_units;
    int first = mxr_desc_first_iram();

    for (int i = first; i < (int)s_desc_count; i++)
    {
        if (!mxr_desc_is_iram(&s_desc[i]))
        {
            break;
        }

        uint16_t off = mxr_desc_off(&s_desc[i]);
        uint16_t len = mxr_desc_len(&s_desc[i]);
        uint32_t block_end = (uint32_t)off + len;

        if (off > cur)
        {
            uint16_t gap = (uint16_t)(off - cur);
            if (gap >= units)
            {
                *out_off_units = cur;
                return true;
            }
        }

        if (block_end > cur)
        {
            cur = (uint16_t)block_end;
        }

        if (cur >= end)
        {
            break;
        }
    }

    if (end > cur)
    {
        uint16_t gap = (uint16_t)(end - cur);
        if (gap >= units)
        {
            *out_off_units = cur;
            return true;
        }
    }

    return false;
}

static bool MXR_IRAM_ATTR mxr_iram_find_free_from_end(
    uint16_t units,
    uint16_t *out_off_units)
{
    if (!s_iram_enabled)
    {
        return false;
    }
    if (units == 0 || units > s_iram_total_units)
    {
        return false;
    }
    if (s_iram_free_units < units)
    {
        return false;
    }

    uint16_t candidate_end = s_iram_total_units;

    for (int i = (int)s_desc_count - 1; i >= 0; i--)
    {
        if (!mxr_desc_is_iram(&s_desc[i]))
        {
            break;
        }

        uint16_t off = mxr_desc_off(&s_desc[i]);
        uint16_t len = mxr_desc_len(&s_desc[i]);
        uint32_t block_end = (uint32_t)off + len;

        if (candidate_end > block_end)
        {
            uint16_t gap = (uint16_t)(candidate_end - (uint16_t)block_end);
            if (gap >= units)
            {
                *out_off_units = (uint16_t)(candidate_end - units);
                return true;
            }
        }

        if (off < candidate_end)
        {
            candidate_end = off;
        }

        if (candidate_end == 0)
        {
            break;
        }
    }

    if (candidate_end > 0)
    {
        uint16_t gap = candidate_end;
        if (gap >= units)
        {
            *out_off_units = (uint16_t)(candidate_end - units);
            return true;
        }
    }

    return false;
}

static uint16_t MXR_IRAM_ATTR mxr_iram_largest_free_units(void)
{
    if (!s_iram_enabled)
    {
        return 0;
    }

    uint16_t cur = 0;
    uint16_t end = s_iram_total_units;
    uint16_t largest = 0;
    int first = mxr_desc_first_iram();

    for (int i = first; i < (int)s_desc_count; i++)
    {
        if (!mxr_desc_is_iram(&s_desc[i]))
        {
            break;
        }

        uint16_t off = mxr_desc_off(&s_desc[i]);
        uint16_t len = mxr_desc_len(&s_desc[i]);
        uint32_t block_end = (uint32_t)off + len;

        if (off > cur)
        {
            uint16_t gap = (uint16_t)(off - cur);
            if (gap > largest)
            {
                largest = gap;
            }
        }

        if (block_end > cur)
        {
            cur = (uint16_t)block_end;
        }

        if (cur >= end)
        {
            break;
        }
    }

    if (end > cur)
    {
        uint16_t gap = (uint16_t)(end - cur);
        if (gap > largest)
        {
            largest = gap;
        }
    }

    return largest;
}

static bool MXR_IRAM_ATTR mxr_caps_allow_iram_fallback(uint32_t caps)
{
    if (!s_iram_enabled)
    {
        return false;
    }
    if (caps & MALLOC_CAP_EXEC)
    {
        return false;
    }
    if (caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM))
    {
        return false;
    }
    if ((caps & MALLOC_CAP_32BIT) || caps == 0)
    {
        return true;
    }
    return false;
}

static inline uint16_t MXR_IRAM_INLINE_ATTR mxr_iram_reserve_units(void)
{
    uint32_t reserve_bytes = CONFIG_MXR_IRAM_RESERVE_BYTES;
    uint32_t reserve_units =
        (reserve_bytes + MXR_UNIT_SIZE - 1) / MXR_UNIT_SIZE;
    if (reserve_units > MXR_MAX_LEN_UNITS)
    {
        reserve_units = MXR_MAX_LEN_UNITS;
    }
    return (uint16_t)reserve_units;
}

static bool MXR_IRAM_ATTR mxr_iram_can_fallback(uint16_t units)
{
    if (!s_iram_enabled)
    {
        return false;
    }
    if (s_iram_free_units < units)
    {
        return false;
    }

    uint32_t need = (uint32_t)units + mxr_iram_reserve_units();
    if (s_iram_free_units < need)
    {
        return false;
    }

    if (CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES != 0)
    {
        uint32_t max_units =
            (CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES + MXR_UNIT_SIZE - 1) /
            MXR_UNIT_SIZE;
        if (units > max_units)
        {
            return false;
        }
    }

    return true;
}

/*
 * Check whether a non-EXEC IRAM fallback block may grow in place.
 *
 * For EXEC blocks reserve is not applied.
 * For non-EXEC fallback blocks we must keep CONFIG_MXR_IRAM_RESERVE_BYTES
 * free for future EXEC allocations.
 */
static bool MXR_IRAM_ATTR mxr_iram_can_grow_fallback(
    uint16_t old_units,
    uint16_t new_units)
{
    if (!s_iram_enabled)
    {
        return false;
    }
    if (new_units <= old_units)
    {
        return true;
    }

    uint16_t extra = (uint16_t)(new_units - old_units);
    uint32_t reserve_units = mxr_iram_reserve_units();
    if (s_iram_free_units < (uint32_t)extra + reserve_units)
    {
        return false;
    }

    if (CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES != 0)
    {
        uint32_t max_units =
            (CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES + MXR_UNIT_SIZE - 1) /
            MXR_UNIT_SIZE;
        if (new_units > max_units)
        {
            return false;
        }
    }

    return true;
}

static void mxr_init_iram(void)
{
    extern char _iram_end;

#ifndef CONFIG_SOC_IRAM_SIZE
#define CONFIG_SOC_IRAM_SIZE 0xC000
#endif

    uint8_t *start = (uint8_t *)(((uint32_t)&_iram_end + 3) & ~3);
    uint8_t *end = (uint8_t *)(0x40100000 + CONFIG_SOC_IRAM_SIZE);

    s_iram_enabled = false;
    s_iram_base = NULL;
    s_iram_total_units = 0;
    s_iram_free_units = 0;
    s_iram_min_free_units = 0;
    s_iram_exec_allocs = 0;
    s_iram_fallback_allocs = 0;

    if (end <= start)
    {
        return;
    }

    size_t bytes = (size_t)(end - start);

    /*
     * Original ESP8266 heap limits:
     *
     *   HEAP_REGION_IRAM_MIN = 512
     *   HEAP_REGION_IRAM_MAX = 0x00010000
     */
    if (bytes <= 512 || bytes >= 0x00010000)
    {
        return;
    }

    size_t units = bytes / MXR_UNIT_SIZE;
    if (units == 0 || units > MXR_MAX_LEN_UNITS)
    {
        return;
    }

    s_iram_base = start;
    s_iram_total_units = (uint16_t)units;
    s_iram_free_units = (uint16_t)units;
    s_iram_min_free_units = (uint16_t)units;
    s_iram_enabled = true;

    ESP_EARLY_LOGI(TAG,
                   "IRAM heap ok: base=%p units=%u bytes=%u",
                   s_iram_base,
                   (unsigned)s_iram_total_units,
                   (unsigned)(s_iram_total_units * MXR_UNIT_SIZE));
}

#endif /* CONFIG_MXR_USE_IRAM */

/* ================================================================
 *  Fallback helpers
 * ================================================================ */

/*
 * Try IRAM fallback for a non-EXEC 32BIT allocation.
 * Returns pointer on success, NULL on failure.
 */
#ifdef CONFIG_MXR_USE_IRAM
static void *MXR_IRAM_ATTR mxr_try_iram_fallback(uint16_t units, uint32_t caps)
{
    if (!mxr_caps_allow_iram_fallback(caps))
    {
        return NULL;
    }
    if (!mxr_iram_can_fallback(units))
    {
        return NULL;
    }

    uint16_t off_units = 0;
    if (!mxr_iram_find_free_from_end(units, &off_units))
    {
        return NULL;
    }

    if (!mxr_desc_insert_ex(off_units, units, true, 0))
    {
        return NULL;
    }

    mxr_iram_allocated(units);
    s_iram_fallback_allocs++;
    s_stats.iram_fallback_allocs++;

    return mxr_iram_units_to_ptr(off_units);
}
#endif /* CONFIG_MXR_USE_IRAM */

#ifdef CONFIG_MXR_CROSS_REGION_FALLBACK

/*
 * Try cross-region DRAM fallback.
 *
 * Strategy:
 *   1. Skip the block's own region.
 *   2. Pick the region with the smallest min_units
 *      that has enough total free space.
 *   3. Try to allocate there.
 *   4. If allocation fails because of fragmentation,
 *      try the next best region.
 *
 * This is a LAST RESORT mechanism.
 *
 * Returns pointer on success, NULL on failure.
 */
static void *MXR_IRAM_ATTR mxr_try_cross_region(
    uint16_t units,
    uint32_t caps,
    int skip_region)
{
    uint32_t tried = 0;

    if (skip_region >= 0 && skip_region < (int)s_region_count)
    {
        tried |= (1u << skip_region);
    }

    for (;;)
    {
        int best = -1;
        uint16_t best_min = 0xFFFF;

        for (uint8_t i = 0; i < s_region_count; i++)
        {
            if (tried & (1u << i))
            {
                continue;
            }
            if (!mxr_region_caps_ok((int)i, caps))
            {
                continue;
            }
            if (s_region[i].free_units < units)
            {
                continue;
            }

#ifdef CONFIG_MXR_CROSS_REGION_CHECK_LARGEST
            if (mxr_region_largest_free_units(i) < units)
            {
                s_stats.cross_region_skip_fragmented++;
                tried |= (1u << i);
                continue;
            }
#endif

            /*
             * Prefer the region whose size class is closest
             * to the requested block size.
             */
            if (s_region[i].min_units < best_min)
            {
                best_min = s_region[i].min_units;
                best = (int)i;
            }
        }

        if (best < 0)
        {
            return NULL;
        }

        tried |= (1u << best);

        uint16_t off_units = 0;
        if (!mxr_try_alloc_region(best, units, &off_units))
        {
            /*
             * This region has enough total free memory,
             * but no contiguous free block.
             *
             * Try the next candidate region.
             */
            continue;
        }

        if (!mxr_desc_insert_ex(off_units, units, false, 0))
        {
            /*
             * Descriptor table full or internal overlap error.
             *
             * If the table is full, trying other regions will
             * not help because every allocation needs a descriptor.
             */
            return NULL;
        }

#ifdef CONFIG_MXR_SEARCH_BITMAP
        mxr_bitmap_set_range(off_units, units);
#endif

        s_region[best].alloc_count++;
        mxr_region_allocated(best, units);
        s_stats.cross_region_allocs++;

        return mxr_units_to_ptr(off_units);
    }
}

#endif /* CONFIG_MXR_CROSS_REGION_FALLBACK */

/* ================================================================
 *  Locked allocation
 * ================================================================ */

static void *MXR_IRAM_ATTR mxr_malloc_caps_locked(size_t size, uint32_t caps)
{
    if (!s_initialized)
    {
        return NULL;
    }

    if (size == 0)
    {
        size = 1;
    }

    if (size > MXR_MAX_LEN_BYTES)
    {
        s_stats.alloc_fail_no_memory++;
        return NULL;
    }

    uint16_t units = mxr_bytes_to_units(size);
    if (units == 0)
    {
        units = 1;
    }
    if (units > MXR_MAX_LEN_UNITS)
    {
        s_stats.alloc_fail_no_memory++;
        return NULL;
    }

#ifdef CONFIG_MXR_USE_IRAM
    /*
     * EXEC allocations go only to IRAM.
     */
    if (caps & MALLOC_CAP_EXEC)
    {
        if ((caps & ~(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)) != 0)
        {
            s_stats.alloc_fail_no_memory++;
            return NULL;
        }
        if (!s_iram_enabled)
        {
            s_stats.alloc_fail_no_memory++;
            return NULL;
        }

        uint16_t off_units = 0;
        if (!mxr_iram_find_free_from_start(units, &off_units))
        {
            s_stats.alloc_fail_no_memory++;
            return NULL;
        }

        if (!mxr_desc_insert_ex(off_units, units, true, MXR_LEN_FLAG_EXEC))
        {
            return NULL;
        }

        mxr_iram_allocated(units);
        s_iram_exec_allocs++;
        s_stats.exec_allocs++;

        return mxr_iram_units_to_ptr(off_units);
    }
#else
    if (caps & MALLOC_CAP_EXEC)
    {
        s_stats.alloc_fail_no_memory++;
        return NULL;
    }
#endif

    /*
     * DRAM allocation.
     *
     * Step 1: try the block's own size-class region.
     */
    int region = mxr_region_for_size(units, caps);
    if (region >= 0)
    {
        uint16_t off_units = 0;
        if (mxr_try_alloc_region(region, units, &off_units))
        {
            if (!mxr_desc_insert_ex(off_units, units, false, 0))
            {
                return NULL;
            }

#ifdef CONFIG_MXR_SEARCH_BITMAP
            mxr_bitmap_set_range(off_units, units);
#endif

            s_region[region].alloc_count++;
            mxr_region_allocated(region, units);

            return mxr_units_to_ptr(off_units);
        }
    }

    /*
     * Step 2: fallback chain.
     *
     * Each stage is compiled out entirely when its feature is
     * disabled, so the hot path never calls a dead fallback.
     *
     * Order when both features are enabled:
     *   CONFIG_MXR_CROSS_REGION_AFTER_IRAM=y (default): IRAM -> cross-region
     *   CONFIG_MXR_CROSS_REGION_AFTER_IRAM=n:           cross-region -> IRAM
     */
#if defined(CONFIG_MXR_USE_IRAM) || defined(CONFIG_MXR_CROSS_REGION_FALLBACK)
    void *fallback_ptr;
#endif

#if defined(CONFIG_MXR_CROSS_REGION_FALLBACK) && \
    !defined(CONFIG_MXR_CROSS_REGION_AFTER_IRAM)
    /*
     * Cross-region BEFORE IRAM.
     */
    fallback_ptr = mxr_try_cross_region(units, caps, region);
    if (fallback_ptr)
    {
        return fallback_ptr;
    }
#ifdef CONFIG_MXR_USE_IRAM
    fallback_ptr = mxr_try_iram_fallback(units, caps);
    if (fallback_ptr)
    {
        return fallback_ptr;
    }
#endif
#else
    /*
     * IRAM BEFORE cross-region (default order).
     */
#ifdef CONFIG_MXR_USE_IRAM
    fallback_ptr = mxr_try_iram_fallback(units, caps);
    if (fallback_ptr)
    {
        return fallback_ptr;
    }
#endif
#ifdef CONFIG_MXR_CROSS_REGION_FALLBACK
    fallback_ptr = mxr_try_cross_region(units, caps, region);
    if (fallback_ptr)
    {
        return fallback_ptr;
    }
#endif
#endif

    s_stats.alloc_fail_no_memory++;
    return NULL;
}

/* ================================================================
 *  Locked free
 * ================================================================ */

static void MXR_IRAM_ATTR mxr_free_locked(void *ptr)
{
    if (!ptr)
    {
        return;
    }

    if (!s_initialized)
    {
        s_stats.invalid_free_attempts++;
        return;
    }

    mxr_arena_id_t arena = mxr_ptr_to_arena(ptr);
    if (arena == MXR_ARENA_NONE)
    {
        s_stats.invalid_free_attempts++;
        return;
    }

    if (arena == MXR_ARENA_DRAM)
    {
        uint16_t off_units = mxr_ptr_to_units(ptr);
        uint16_t key = mxr_desc_make_key(off_units, false);
        int index = mxr_desc_find_key(key);
        if (index < 0)
        {
            s_stats.invalid_free_attempts++;
            return;
        }

        uint16_t len_units = mxr_desc_len(&s_desc[index]);
        int region = mxr_region_by_off(off_units);

#ifdef CONFIG_MXR_SEARCH_BITMAP
        mxr_bitmap_clear_range(off_units, len_units);
#endif

        mxr_desc_remove(index);

        if (region >= 0)
        {
            if (s_region[region].alloc_count > 0)
            {
                s_region[region].alloc_count--;
            }
        }

        mxr_region_released(region, len_units);
        return;
    }

#ifdef CONFIG_MXR_USE_IRAM
    if (arena == MXR_ARENA_IRAM)
    {
        uint16_t off_units = mxr_iram_ptr_to_units(ptr);
        uint16_t key = mxr_desc_make_key(off_units, true);
        int index = mxr_desc_find_key(key);
        if (index < 0)
        {
            s_stats.invalid_free_attempts++;
            return;
        }

        uint16_t len_units = mxr_desc_len(&s_desc[index]);

        mxr_desc_remove(index);
        mxr_iram_released(len_units);
        return;
    }
#endif
}

/* ================================================================
 *  Public API
 * ================================================================ */

void *MXR_IRAM_ATTR mxr_malloc_caps(size_t size, uint32_t caps)
{
    mxr_lock();
    void *p = mxr_malloc_caps_locked(size, caps);
    mxr_unlock();
    return p;
}

void MXR_IRAM_ATTR mxr_free(void *ptr)
{
    mxr_lock();
    mxr_free_locked(ptr);
    mxr_unlock();
}

void *MXR_IRAM_ATTR mxr_malloc(size_t size)
{
    return mxr_malloc_caps(size, MALLOC_CAP_32BIT);
}

void *MXR_IRAM_ALLOC_ATTR mxr_calloc_caps(size_t count, size_t size, uint32_t caps)
{
    size_t total_bytes;
    if (__builtin_mul_overflow(count, size, &total_bytes))
    {
        mxr_lock();
        s_stats.alloc_fail_no_memory++;
        mxr_unlock();
        return NULL;
    }

    void *ptr = mxr_malloc_caps(total_bytes, caps);
    if (ptr)
    {
        uint16_t clear_units = mxr_bytes_to_units(total_bytes ? total_bytes : 1);
        mxr_memset_words(ptr, clear_units);
    }
    return ptr;
}

void *MXR_IRAM_ALLOC_ATTR mxr_calloc(size_t count, size_t size)
{
    return mxr_calloc_caps(count, size, MALLOC_CAP_32BIT);
}

void *MXR_IRAM_ALLOC_ATTR mxr_zalloc_caps(size_t size, uint32_t caps)
{
    void *ptr = mxr_malloc_caps(size, caps);
    if (ptr)
    {
        uint16_t clear_units = mxr_bytes_to_units(size ? size : 1);
        mxr_memset_words(ptr, clear_units);
    }
    return ptr;
}

void *MXR_IRAM_ALLOC_ATTR mxr_zalloc(size_t size)
{
    return mxr_zalloc_caps(size, MALLOC_CAP_32BIT);
}

/* ================================================================
 *  Realloc
 * ================================================================ */

void *MXR_IRAM_ALLOC_ATTR mxr_realloc_caps(void *ptr, size_t newsize, uint32_t caps)
{
    if (!ptr)
    {
        return mxr_malloc_caps(newsize, caps);
    }

    if (!s_initialized)
    {
        return NULL;
    }

    if (newsize == 0)
    {
#if MXR_REALLOC_ZERO_FREES
        mxr_lock();
        mxr_free_locked(ptr);
        mxr_unlock();
        return NULL;
#else
        newsize = 1;
#endif
    }

    if (newsize > MXR_MAX_LEN_BYTES)
    {
        return NULL;
    }

    uint16_t new_units = mxr_bytes_to_units(newsize);
    if (new_units == 0)
    {
        new_units = 1;
    }
    if (new_units > MXR_MAX_LEN_UNITS)
    {
        return NULL;
    }

    mxr_lock();

    mxr_arena_id_t arena = mxr_ptr_to_arena(ptr);
    if (arena == MXR_ARENA_NONE)
    {
        s_stats.invalid_free_attempts++;
        mxr_unlock();
        return NULL;
    }

    /*
     * DRAM realloc.
     */
    if (arena == MXR_ARENA_DRAM)
    {
        uint16_t off_units = mxr_ptr_to_units(ptr);
        uint16_t key = mxr_desc_make_key(off_units, false);
        int index = mxr_desc_find_key(key);
        if (index < 0)
        {
            s_stats.invalid_free_attempts++;
            mxr_unlock();
            return NULL;
        }

        uint16_t old_units = mxr_desc_len(&s_desc[index]);
        int region = mxr_region_by_off(off_units);

        bool caps_ok = mxr_region_caps_ok(region, caps);
        bool in_place_allowed = caps_ok && mxr_region_size_ok(region, new_units);

        /*
         * Same size.
         */
        if (new_units == old_units && region >= 0 && in_place_allowed)
        {
            mxr_unlock();
            return ptr;
        }

        /*
         * Shrink in place.
         */
        if (new_units < old_units && region >= 0 && in_place_allowed)
        {
            uint16_t diff = (uint16_t)(old_units - new_units);

#ifdef CONFIG_MXR_SEARCH_BITMAP
            mxr_bitmap_clear_range((uint16_t)(off_units + new_units), diff);
#endif

            mxr_desc_set_len(&s_desc[index], new_units);
            mxr_region_released(region, diff);

            mxr_unlock();
            return ptr;
        }

        /*
         * Grow in place.
         */
        if (new_units > old_units && region >= 0 && in_place_allowed)
        {
            uint16_t extra = (uint16_t)(new_units - old_units);
            uint32_t block_end = (uint32_t)off_units + old_units;
            uint16_t region_end =
                (uint16_t)(s_region[region].start_unit + s_region[region].total_units);

            uint32_t next_boundary;
            if (index + 1 < s_desc_count &&
                !mxr_desc_is_iram(&s_desc[index + 1]))
            {
                uint16_t next_off = mxr_desc_off(&s_desc[index + 1]);
                if (next_off < region_end)
                {
                    next_boundary = next_off;
                }
                else
                {
                    next_boundary = region_end;
                }
            }
            else
            {
                next_boundary = region_end;
            }

            if (next_boundary >= block_end)
            {
                uint32_t gap = next_boundary - block_end;
                if (gap >= extra)
                {
#ifdef CONFIG_MXR_SEARCH_BITMAP
                    mxr_bitmap_set_range((uint16_t)(off_units + old_units), extra);
#endif

                    mxr_desc_set_len(&s_desc[index], new_units);
                    mxr_region_allocated(region, extra);

                    mxr_unlock();
                    return ptr;
                }
            }
        }

        uint16_t copy_words = (old_units < new_units) ? old_units : new_units;

        void *new_ptr = mxr_malloc_caps_locked(newsize, caps);
        if (!new_ptr)
        {
            mxr_unlock();
            return NULL;
        }

        /*
         * Copy outside the lock.
         * Caller guarantees no concurrent access to ptr during realloc.
         */
        mxr_unlock();

        if (new_ptr != ptr)
        {
            mxr_memcpy_words(new_ptr, ptr, copy_words);
        }

        mxr_lock();
        if (new_ptr != ptr)
        {
            mxr_free_locked(ptr);
        }
        mxr_unlock();

        return new_ptr;
    }

#ifdef CONFIG_MXR_USE_IRAM
    /*
     * IRAM realloc.
     */
    if (arena == MXR_ARENA_IRAM)
    {
        uint16_t off_units = mxr_iram_ptr_to_units(ptr);
        uint16_t key = mxr_desc_make_key(off_units, true);
        int index = mxr_desc_find_key(key);
        if (index < 0)
        {
            s_stats.invalid_free_attempts++;
            mxr_unlock();
            return NULL;
        }

        bool old_exec = mxr_desc_is_exec(&s_desc[index]);

        /*
         * An existing EXEC block must remain EXEC.
         */
        if (old_exec && !(caps & MALLOC_CAP_EXEC))
        {
            caps |= MALLOC_CAP_EXEC;
        }

        uint16_t old_units = mxr_desc_len(&s_desc[index]);
        bool want_exec = (caps & MALLOC_CAP_EXEC) != 0;

        bool in_place_allowed = false;
        if (want_exec)
        {
            /*
             * EXEC in-place is allowed only if the old block is already EXEC.
             *
             * Do not silently convert a non-EXEC fallback block into EXEC.
             * If caller wants EXEC from a non-EXEC block, force move.
             */
            in_place_allowed =
                old_exec &&
                ((caps & ~(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)) == 0);
        }
        else
        {
            /*
             * Non-EXEC IRAM fallback block can be reallocated in-place
             * only if the new caps still allow IRAM fallback.
             *
             * This keeps DMA / 8BIT / SPIRAM requests out of IRAM.
             */
            in_place_allowed =
                !old_exec &&
                mxr_caps_allow_iram_fallback(caps);
        }

        if (in_place_allowed)
        {
            /*
             * Same size.
             */
            if (new_units == old_units)
            {
                mxr_unlock();
                return ptr;
            }

            /*
             * Shrink in place.
             */
            if (new_units < old_units)
            {
                uint16_t diff = (uint16_t)(old_units - new_units);

                mxr_desc_set_len(&s_desc[index], new_units);
                mxr_iram_released(diff);

                mxr_unlock();
                return ptr;
            }

            /*
             * Grow in place.
             */
            uint16_t extra = (uint16_t)(new_units - old_units);

            /*
             * Non-EXEC fallback growth must respect IRAM reserve
             * and fallback block size limit.
             */
            if (!old_exec)
            {
                if (!mxr_iram_can_grow_fallback(old_units, new_units))
                {
                    in_place_allowed = false;
                }
            }

            if (in_place_allowed)
            {
                uint32_t block_end = (uint32_t)off_units + old_units;

                uint32_t next_boundary;
                if (index + 1 < s_desc_count &&
                    mxr_desc_is_iram(&s_desc[index + 1]))
                {
                    next_boundary = mxr_desc_off(&s_desc[index + 1]);
                }
                else
                {
                    next_boundary = s_iram_total_units;
                }

                if (next_boundary >= block_end)
                {
                    uint32_t gap = next_boundary - block_end;
                    if (gap >= extra)
                    {
                        mxr_desc_set_len(&s_desc[index], new_units);
                        mxr_iram_allocated(extra);

                        mxr_unlock();
                        return ptr;
                    }
                }
            }
        }

        /*
         * Cannot resize in place.
         * Allocate a new block, copy data, then free the old block.
         */
        uint16_t copy_words = (old_units < new_units) ? old_units : new_units;

        void *new_ptr = mxr_malloc_caps_locked(newsize, caps);
        if (!new_ptr)
        {
            mxr_unlock();
            return NULL;
        }

        /*
         * Copy outside the lock.
         * Caller guarantees no concurrent access to ptr during realloc.
         */
        mxr_unlock();

        if (new_ptr != ptr)
        {
            mxr_memcpy_words(new_ptr, ptr, copy_words);
        }

        mxr_lock();
        if (new_ptr != ptr)
        {
            mxr_free_locked(ptr);
        }
        mxr_unlock();

        return new_ptr;
    }
#endif

    mxr_unlock();
    return NULL;
}

void *MXR_IRAM_ALLOC_ATTR mxr_realloc(void *ptr, size_t newsize)
{
    return mxr_realloc_caps(ptr, newsize, MALLOC_CAP_32BIT);
}

/* ================================================================
 *  Region initialization
 * ================================================================ */

static void mxr_init_regions_temp_single(void)
{
    memset(s_region, 0, sizeof(s_region));

    s_region_count = 1;

    s_region[0].caps = MXR_DRAM_CAPS_DEFAULT;
    s_region[0].start_unit = 0;
    s_region[0].total_units = s_arena_total_units;
    s_region[0].min_units = 1;
    s_region[0].max_units = MXR_REGION_MAX_UNLIMITED;
    s_region[0].free_units = s_arena_total_units;
    s_region[0].min_free_units = s_arena_total_units;
    s_region[0].alloc_count = 0;
}

static bool mxr_init_regions_exact(
    const mxr_region_cfg_t *cfg,
    uint8_t count)
{
    if (count < 2 || count > MXR_ACTIVE_TOTAL_REGIONS)
    {
        return false;
    }

    memset(s_region, 0, sizeof(s_region));
    s_region_count = 0;

    uint16_t percent_sum = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        percent_sum += cfg[i].percent;
    }

    if (percent_sum > 100)
    {
        ESP_EARLY_LOGE(TAG,
                       "region percent sum must be <= 100, got %u",
                       (unsigned)percent_sum);
        return false;
    }

    /*
     * Sanity check size classes.
     */
    uint16_t expected_min = 0;

    for (uint8_t i = 0; i < count; i++)
    {
        uint16_t min_units = cfg[i].min_units;
        uint16_t max_units = cfg[i].max_units;

        if (min_units == 0)
        {
            min_units = 1;
        }

        if (min_units > MXR_MAX_LEN_UNITS)
        {
            ESP_EARLY_LOGE(TAG,
                           "region %u min too large: %u",
                           (unsigned)i,
                           (unsigned)min_units);
            return false;
        }

        if (max_units == MXR_REGION_MAX_UNLIMITED &&
            i != (uint8_t)(count - 1))
        {
            ESP_EARLY_LOGE(TAG,
                           "only last region may be unlimited: region %u",
                           (unsigned)i);
            return false;
        }

        if (max_units != MXR_REGION_MAX_UNLIMITED)
        {
            if (max_units > MXR_MAX_LEN_UNITS)
            {
                max_units = MXR_MAX_LEN_UNITS;
            }

            if (min_units > max_units)
            {
                ESP_EARLY_LOGE(TAG,
                               "region %u bad min/max: %u/%u",
                               (unsigned)i,
                               (unsigned)min_units,
                               (unsigned)max_units);
                return false;
            }
        }

        if (i > 0)
        {
            if (min_units < expected_min)
            {
                ESP_EARLY_LOGE(TAG,
                               "region %u overlaps previous: min=%u expected=%u",
                               (unsigned)i,
                               (unsigned)min_units,
                               (unsigned)expected_min);
                return false;
            }

            if (min_units > expected_min)
            {
                ESP_EARLY_LOGE(TAG,
                               "gap before region %u: expected=%u min=%u",
                               (unsigned)i,
                               (unsigned)expected_min,
                               (unsigned)min_units);
                return false;
            }
        }

        if (max_units == MXR_REGION_MAX_UNLIMITED ||
            max_units >= MXR_MAX_LEN_UNITS)
        {
            expected_min = MXR_MAX_LEN_UNITS;
        }
        else
        {
            expected_min = (uint16_t)(max_units + 1);
        }
    }

    /*
     * Allocate memory to regions.
     */
    uint16_t remaining_units = s_arena_total_units;

    for (uint8_t i = 0; i < count; i++)
    {
        uint16_t min_units = cfg[i].min_units;
        uint16_t max_units = cfg[i].max_units;

        if (min_units == 0)
        {
            min_units = 1;
        }

        if (max_units != MXR_REGION_MAX_UNLIMITED &&
            max_units > MXR_MAX_LEN_UNITS)
        {
            max_units = MXR_MAX_LEN_UNITS;
        }

        uint16_t units;

        if (i == (uint8_t)(count - 1) && cfg[i].percent == 0)
        {
            units = remaining_units;
        }
        else
        {
            units = (uint16_t)(((uint32_t)s_arena_total_units * cfg[i].percent) / 100);
        }

        if (units < min_units)
        {
            units = min_units;
        }

        if (units > remaining_units)
        {
            ESP_EARLY_LOGE(TAG,
                           "region %u too large: %u > %u",
                           (unsigned)i,
                           (unsigned)units,
                           (unsigned)remaining_units);
            return false;
        }

        s_region[s_region_count].caps = MXR_DRAM_CAPS_DEFAULT;
        s_region[s_region_count].start_unit =
            (uint16_t)(s_arena_total_units - remaining_units);
        s_region[s_region_count].total_units = units;
        s_region[s_region_count].min_units = min_units;
        s_region[s_region_count].max_units = max_units;
        s_region[s_region_count].free_units = units;
        s_region[s_region_count].min_free_units = units;
        s_region[s_region_count].alloc_count = 0;

        remaining_units = (uint16_t)(remaining_units - units);
        s_region_count++;
    }

    /*
     * Add leftover to last region.
     */
    if (remaining_units > 0)
    {
        s_region[count - 1].total_units =
            (uint16_t)(s_region[count - 1].total_units + remaining_units);
        s_region[count - 1].free_units =
            s_region[count - 1].total_units;
        s_region[count - 1].min_free_units =
            s_region[count - 1].free_units;
    }

    return true;
}

/* ================================================================
 *  Custom Kconfig parser
 * ================================================================ */

static const char *mxr_next_percent(const char *p, uint8_t *value)
{
    if (!p)
    {
        return NULL;
    }

    while (*p == ' ' || *p == '\t' || *p == ',')
    {
        p++;
    }

    if (*p == '\0')
    {
        return NULL;
    }

    uint16_t v = 0;
    bool has_digit = false;

    while (*p >= '0' && *p <= '9')
    {
        v = (uint16_t)(v * 10 + (uint16_t)(*p - '0'));
        has_digit = true;
        p++;

        if (v > 100)
        {
            return NULL;
        }
    }

    if (!has_digit)
    {
        return NULL;
    }

    *value = (uint8_t)v;
    return p;
}

static const char *mxr_parse_boundaries(
    const char *s,
    uint16_t *out_bytes,
    uint8_t max_count,
    uint8_t *out_count)
{
    const char *p = s;
    uint8_t count = 0;

    while (count < max_count)
    {
        while (*p == ' ' || *p == '\t' || *p == ',')
        {
            p++;
        }

        if (*p == '\0')
        {
            break;
        }

        uint32_t value = 0;
        bool has_digit = false;

        while (*p >= '0' && *p <= '9')
        {
            value = value * 10 + (uint32_t)(*p - '0');
            has_digit = true;
            p++;

            if (value > 0xFFFF)
            {
                p = NULL;
                break;
            }
        }

        if (!has_digit)
        {
            p = NULL;
            break;
        }

        out_bytes[count] = (uint16_t)value;
        count++;
    }

    *out_count = count;
    return p;
}

static bool mxr_has_extra_csv(const char *p)
{
    if (!p)
    {
        return false;
    }

    while (*p == ' ' || *p == '\t' || *p == ',')
    {
        p++;
    }

    return *p != '\0';
}

static const char *mxr_parse_percent(
    const char *s,
    mxr_region_cfg_t *out,
    uint8_t max_count,
    uint8_t *out_count)
{
    const char *p = s;
    uint8_t count = 0;

    while (count < max_count)
    {
        p = mxr_next_percent(p, &out[count].percent);
        if (!p)
        {
            break;
        }
        count++;
    }

    *out_count = count;
    return p;
}

static bool mxr_init_regions_kconfig(void)
{
    uint8_t total = MXR_USER_REGIONS;

    /*
     * Single flat region mode.
     *
     * If the user selects 1 region, we bypass the CSV parser
     * entirely and create one region that spans the whole arena.
     */
    if (total == 1)
    {
        mxr_init_regions_temp_single();
        return true;
    }

    if (total < 2 || total > MXR_ACTIVE_TOTAL_REGIONS)
    {
        return false;
    }
    
    mxr_region_cfg_t cfg[MXR_ACTIVE_TOTAL_REGIONS];
    uint16_t boundary_bytes[MXR_ACTIVE_TOTAL_REGIONS];

    uint8_t percent_count = 0;
    uint8_t boundary_count = 0;

    const char *percent_end =
        mxr_parse_percent(CONFIG_MXR_REGION_PERCENTS, cfg, total, &percent_count);

    const char *boundary_end =
        mxr_parse_boundaries(CONFIG_MXR_REGION_SIZES, boundary_bytes, total, &boundary_count);

    if (percent_count != total)
    {
        ESP_EARLY_LOGW(TAG,
                       "percent count mismatch: got %u, expected %u",
                       (unsigned)percent_count,
                       (unsigned)total);
        return false;
    }

    if (boundary_count != total)
    {
        ESP_EARLY_LOGW(TAG,
                       "boundary count mismatch: got %u, expected %u",
                       (unsigned)boundary_count,
                       (unsigned)total);
        return false;
    }

    if (mxr_has_extra_csv(percent_end) || mxr_has_extra_csv(boundary_end))
    {
        ESP_EARLY_LOGW(TAG,
                       "extra CSV values detected: expected %u entries",
                       (unsigned)total);
        return false;
    }

    /*
     * Convert byte boundaries to 4-byte units.
     */
    uint16_t boundary_units[MXR_ACTIVE_TOTAL_REGIONS];

    for (uint8_t i = 0; i < total; i++)
    {
        uint16_t bytes = boundary_bytes[i];

        if (bytes < MXR_UNIT_SIZE)
        {
            bytes = MXR_UNIT_SIZE;
        }

        uint16_t units =
            (uint16_t)(((uint32_t)bytes + MXR_UNIT_SIZE - 1) / MXR_UNIT_SIZE);

        if (units == 0)
        {
            units = 1;
        }

        if (units > MXR_MAX_LEN_UNITS)
        {
            units = MXR_MAX_LEN_UNITS;
        }

        boundary_units[i] = units;
    }

    /*
     * Boundaries must be strictly increasing.
     */
    for (uint8_t i = 1; i < total; i++)
    {
        if (boundary_units[i] <= boundary_units[i - 1])
        {
            ESP_EARLY_LOGE(TAG,
                           "boundaries must be strictly increasing: "
                           "b[%u]=%u, b[%u]=%u",
                           (unsigned)(i - 1),
                           (unsigned)boundary_units[i - 1],
                           (unsigned)i,
                           (unsigned)boundary_units[i]);
            return false;
        }
    }

    /*
     * Build region config.
     */
    for (uint8_t i = 0; i < total; i++)
    {
        cfg[i].min_units = boundary_units[i];

        if (i == (uint8_t)(total - 1))
        {
            cfg[i].max_units = MXR_REGION_MAX_UNLIMITED;
        }
        else
        {
            cfg[i].max_units = (uint16_t)(boundary_units[i + 1] - 1);
        }
    }

    return mxr_init_regions_exact(cfg, total);
}

/* ================================================================
 *  Init
 * ================================================================ */

void mxr_init(void)
{
    if (s_initialized)
    {
        return;
    }

    extern char _bss_end;

    uint8_t *start = (uint8_t *)(((uint32_t)&_bss_end + 3) & ~3);
    uint8_t *end = (uint8_t *)0x40000000;

    s_initialized = false;

    if (end <= start)
    {
        ESP_EARLY_LOGE(TAG, "invalid heap bounds");
        return;
    }

    size_t bytes = (size_t)(end - start);
    size_t units = bytes / MXR_UNIT_SIZE;

    if (units > MXR_MAX_ARENA_UNITS)
    {
        ESP_EARLY_LOGE(TAG, "arena too large: %u units", (unsigned)units);
        return;
    }

    s_arena_base = start;
    s_arena_total_units = (uint16_t)units;

#ifdef CONFIG_MXR_SEARCH_BITMAP
    /*
     * Carve bitmap from the end of the arena.
     *
     * 1 bit per unit.
     * bitmap_words = ceil(arena_units / 32)
     * Each word = 4 bytes = 1 unit.
     */
    {
        uint16_t full_units = s_arena_total_units;
        uint16_t bitmap_words = (uint16_t)((full_units + 31u) / 32u);

        s_bitmap_units = bitmap_words;
        s_arena_total_units = (uint16_t)(full_units - bitmap_words);

        s_bitmap = (uint32_t *)(s_arena_base +
                                (size_t)s_arena_total_units * MXR_UNIT_SIZE);

        memset(s_bitmap, 0, (size_t)s_bitmap_units * MXR_UNIT_SIZE);
    }
#endif

    memset(s_desc, 0, sizeof(s_desc));
    s_desc_count = 0;

    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.desc_capacity = CONFIG_MXR_MAX_DESC;

#ifdef CONFIG_MXR_USE_IRAM
    mxr_init_iram();
#endif

    bool regions_ok = mxr_init_regions_kconfig();
    if (!regions_ok)
    {
        ESP_EARLY_LOGW(TAG, "region init failed, using single region");
        mxr_init_regions_temp_single();
    }

    uint16_t largest_units = 0;
    for (uint8_t i = 0; i < s_region_count; i++)
    {
        if (s_region[i].total_units > largest_units)
        {
            largest_units = s_region[i].total_units;
        }
    }

    size_t total_bytes = (size_t)s_arena_total_units * MXR_UNIT_SIZE;
    size_t free_bytes = total_bytes;

    s_stats.iram_total_bytes = 0;
    s_stats.iram_free_bytes = 0;
    s_stats.iram_min_free_bytes = 0;
    s_stats.exec_allocs = 0;
    s_stats.iram_fallback_allocs = 0;

#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        size_t iram_bytes = (size_t)s_iram_total_units * MXR_UNIT_SIZE;

        total_bytes += iram_bytes;
        free_bytes += iram_bytes;

        s_stats.iram_total_bytes = iram_bytes;
        s_stats.iram_free_bytes = iram_bytes;
        s_stats.iram_min_free_bytes = iram_bytes;

        uint16_t iram_largest = mxr_iram_largest_free_units();
        if (iram_largest > largest_units)
        {
            largest_units = iram_largest;
        }
    }
#endif

    s_stats.initialized = true;
    s_stats.region_count = s_region_count;
    s_stats.total_bytes = total_bytes;
    s_stats.free_bytes = free_bytes;
    s_stats.min_free_bytes = total_bytes;
    s_stats.largest_free_block_bytes = (size_t)largest_units * MXR_UNIT_SIZE;

    s_initialized = true;

    ESP_EARLY_LOGI(TAG,
                   "init ok: base=%p units=%u bytes=%u",
                   s_arena_base,
                   (unsigned)s_arena_total_units,
                   (unsigned)((size_t)s_arena_total_units * MXR_UNIT_SIZE));
}

/* ================================================================
 *  Status
 * ================================================================ */

void mxr_get_status(mxr_status_t *status)
{
    if (!status)
    {
        return;
    }

    mxr_lock();

    s_stats.active_allocs = s_desc_count;
    s_stats.region_count = s_region_count;

    size_t total_bytes = 0;
    size_t free_bytes = 0;
    uint16_t largest_units = 0;

    for (uint8_t i = 0; i < s_region_count; i++)
    {
        total_bytes += (size_t)s_region[i].total_units * MXR_UNIT_SIZE;
        free_bytes += (size_t)s_region[i].free_units * MXR_UNIT_SIZE;

        uint16_t largest_region = mxr_region_largest_free_units(i);
        if (largest_region > largest_units)
        {
            largest_units = largest_region;
        }
    }

    s_stats.iram_total_bytes = 0;
    s_stats.iram_free_bytes = 0;
    s_stats.iram_min_free_bytes = 0;
    s_stats.exec_allocs = 0;
    s_stats.iram_fallback_allocs = 0;

#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        total_bytes += (size_t)s_iram_total_units * MXR_UNIT_SIZE;
        free_bytes += (size_t)s_iram_free_units * MXR_UNIT_SIZE;

        s_stats.iram_total_bytes =
            (size_t)s_iram_total_units * MXR_UNIT_SIZE;
        s_stats.iram_free_bytes =
            (size_t)s_iram_free_units * MXR_UNIT_SIZE;
        s_stats.iram_min_free_bytes =
            (size_t)s_iram_min_free_units * MXR_UNIT_SIZE;

        s_stats.exec_allocs = s_iram_exec_allocs;
        s_stats.iram_fallback_allocs = s_iram_fallback_allocs;

        uint16_t iram_largest = mxr_iram_largest_free_units();
        if (iram_largest > largest_units)
        {
            largest_units = iram_largest;
        }
    }
#endif

    s_stats.total_bytes = total_bytes;
    s_stats.free_bytes = free_bytes;
    s_stats.largest_free_block_bytes = (size_t)largest_units * MXR_UNIT_SIZE;

    if (s_stats.free_bytes < s_stats.min_free_bytes)
    {
        s_stats.min_free_bytes = s_stats.free_bytes;
    }

    *status = s_stats;

    mxr_unlock();
}

bool mxr_get_region_status(int region_index, mxr_region_status_t *status)
{
    if (!status)
    {
        return false;
    }
    if (region_index < 0 || region_index >= s_region_count)
    {
        return false;
    }

    mxr_lock();

    uint8_t i = (uint8_t)region_index;

    status->caps = s_region[i].caps;
    status->start_unit = s_region[i].start_unit;
    status->total_units = s_region[i].total_units;
    status->min_units = s_region[i].min_units;
    status->max_units = s_region[i].max_units;
    status->free_units = s_region[i].free_units;
    status->min_free_units = s_region[i].min_free_units;
    status->largest_free_units = mxr_region_largest_free_units(i);
    status->alloc_count = s_region[i].alloc_count;

    mxr_unlock();

    return true;
}

size_t mxr_get_free_size_caps(uint32_t caps)
{
    if (!s_initialized)
    {
        return 0;
    }

    size_t bytes = 0;

    mxr_lock();

    for (uint8_t i = 0; i < s_region_count; i++)
    {
        if (mxr_region_caps_ok(i, caps))
        {
            bytes += (size_t)s_region[i].free_units * MXR_UNIT_SIZE;
        }
    }

#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        bool iram_ok = false;

        if (caps == 0)
        {
            iram_ok = true;
        }
        else if (caps & MALLOC_CAP_EXEC)
        {
            if ((caps & ~(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)) == 0)
            {
                iram_ok = true;
            }
        }
        else if ((caps & MALLOC_CAP_32BIT) &&
                 !(caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM)))
        {
            iram_ok = true;
        }

        if (iram_ok)
        {
            if (caps & MALLOC_CAP_EXEC)
            {
                bytes += (size_t)s_iram_min_free_units * MXR_UNIT_SIZE;
            }
            else
            {
                uint16_t reserve = mxr_iram_reserve_units();
                if (s_iram_min_free_units > reserve)
                {
                    bytes += (size_t)(s_iram_min_free_units - reserve) * MXR_UNIT_SIZE;
                }
            }
        }
    }
#endif

    mxr_unlock();

    return bytes;
}

size_t mxr_get_min_free_size_caps(uint32_t caps)
{
    if (!s_initialized)
    {
        return 0;
    }

    size_t bytes = 0;

    mxr_lock();

    for (uint8_t i = 0; i < s_region_count; i++)
    {
        if (mxr_region_caps_ok(i, caps))
        {
            bytes += (size_t)s_region[i].min_free_units * MXR_UNIT_SIZE;
        }
    }

#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        bool iram_ok = false;

        if (caps == 0)
        {
            iram_ok = true;
        }
        else if (caps & MALLOC_CAP_EXEC)
        {
            if ((caps & ~(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)) == 0)
            {
                iram_ok = true;
            }
        }
        else if ((caps & MALLOC_CAP_32BIT) &&
                 !(caps & (MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM)))
        {
            iram_ok = true;
        }

        if (iram_ok)
        {
            if (caps & MALLOC_CAP_EXEC)
            {
                bytes += (size_t)s_iram_min_free_units * MXR_UNIT_SIZE;
            }
            else
            {
                uint16_t reserve = mxr_iram_reserve_units();
                if (s_iram_min_free_units > reserve)
                {
                    bytes += (size_t)(s_iram_min_free_units - reserve) * MXR_UNIT_SIZE;
                }
            }
        }
    }
#endif

    mxr_unlock();

    return bytes;
}

/* ================================================================
 *  Dump
 * ================================================================ */

void mxr_dump(void)
{
    mxr_status_t st;
    mxr_get_status(&st);

    ESP_EARLY_LOGI(TAG, "MxR dump: initialized=%d", (int)st.initialized);

#ifdef CONFIG_MXR_SEARCH_BITMAP
    ESP_EARLY_LOGI(TAG, "search mode: bitmap");
#else
    ESP_EARLY_LOGI(TAG, "search mode: descriptor");
#endif

    if (!st.initialized)
    {
        return;
    }

    ESP_EARLY_LOGI(TAG,
                   "total=%u free=%u min_free=%u largest=%u",
                   (unsigned)st.total_bytes,
                   (unsigned)st.free_bytes,
                   (unsigned)st.min_free_bytes,
                   (unsigned)st.largest_free_block_bytes);

    ESP_EARLY_LOGI(TAG,
                   "desc used=%u/%u max_used=%u",
                   (unsigned)st.active_allocs,
                   (unsigned)st.desc_capacity,
                   (unsigned)st.max_active_allocs);

    ESP_EARLY_LOGI(TAG,
                   "exec_allocs=%u iram_fallback=%u cross_region=%u cross_skip_frag=%u",
                   (unsigned)st.exec_allocs,
                   (unsigned)st.iram_fallback_allocs,
                   (unsigned)st.cross_region_allocs,
                   (unsigned)st.cross_region_skip_fragmented);

#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        ESP_EARLY_LOGI(TAG,
                       "IRAM: base=%p total=%u free=%u min_free=%u",
                       s_iram_base,
                       (unsigned)s_iram_total_units,
                       (unsigned)s_iram_free_units,
                       (unsigned)s_iram_min_free_units);
    }
    else
    {
        ESP_EARLY_LOGI(TAG, "IRAM: disabled");
    }
#endif

    for (uint8_t i = 0; i < st.region_count && i < MXR_ACTIVE_TOTAL_REGIONS; i++)
    {
        mxr_region_status_t rs;
        if (!mxr_get_region_status(i, &rs))
        {
            continue;
        }

        ESP_EARLY_LOGI(TAG,
                       "region %u: caps=0x%08x start=%u total=%u min=%u max=%u "
                       "free=%u min_free=%u largest=%u alloc=%u",
                       (unsigned)i,
                       (unsigned)rs.caps,
                       (unsigned)rs.start_unit,
                       (unsigned)rs.total_units,
                       (unsigned)rs.min_units,
                       (unsigned)rs.max_units,
                       (unsigned)rs.free_units,
                       (unsigned)rs.min_free_units,
                       (unsigned)rs.largest_free_units,
                       (unsigned)rs.alloc_count);
    }

    static mxr_desc_t snapshot[CONFIG_MXR_MAX_DESC];
    uint16_t desc_count = 0;

    mxr_lock();
    desc_count = s_desc_count;
    if (desc_count > CONFIG_MXR_MAX_DESC)
    {
        desc_count = CONFIG_MXR_MAX_DESC;
    }
    memcpy(snapshot, s_desc, (size_t)desc_count * sizeof(mxr_desc_t));
    mxr_unlock();

    for (uint16_t i = 0; i < desc_count; i++)
    {
        ESP_EARLY_LOGI(TAG,
                       "desc[%u]: off=%u len=%u iram=%d exec=%d",
                       (unsigned)i,
                       (unsigned)mxr_desc_off(&snapshot[i]),
                       (unsigned)mxr_desc_len(&snapshot[i]),
                       (int)mxr_desc_is_iram(&snapshot[i]),
                       (int)mxr_desc_is_exec(&snapshot[i]));
    }

    ESP_EARLY_LOGI(TAG,
                   "stats: fail_mem=%u fail_table=%u invalid_free=%u max_allocs=%u",
                   (unsigned)st.alloc_fail_no_memory,
                   (unsigned)st.alloc_fail_table_full,
                   (unsigned)st.invalid_free_attempts,
                   (unsigned)st.max_active_allocs);
}