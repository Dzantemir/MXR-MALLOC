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
#include <stdio.h>

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

/* ================================================================
 *  Allocator state
 * ================================================================ */

/*
 * Descriptor table placement.
 *
 * MXR_DESC_IN_DRAM      : regular .bss, safest, no linker mods
 * MXR_DESC_IN_IRAM_TEXT : .iram0.text, works without linker patch,
 *                         table loaded from flash at boot (PROGBITS)
 * MXR_DESC_IN_IRAM_BSS  : .iram0.bss, requires patched linker script
 *                         with *(.iram0.bss .iram0.bss.*) wildcard,
 *                         table NOT in binary (NOLOAD)
 */
static mxr_desc_t s_desc[CONFIG_MXR_MAX_DESC] MXR_IRAM_DATA_ATTR;

static uint16_t s_desc_count MXR_IRAM_DATA_ATTR;
static uint8_t s_region_count MXR_IRAM_DATA_ATTR;
static uint8_t *s_arena_base MXR_IRAM_DATA_ATTR;
static mxr_size_t s_arena_total_bytes MXR_IRAM_DATA_ATTR;
static bool s_initialized MXR_IRAM_DATA_ATTR;
static mxr_status_t s_stats MXR_IRAM_DATA_ATTR;

#ifdef CONFIG_MXR_USE_IRAM
static bool s_iram_enabled MXR_IRAM_DATA_ATTR;
static uint8_t *s_iram_base MXR_IRAM_DATA_ATTR;
static mxr_size_t s_iram_total_bytes MXR_IRAM_DATA_ATTR;
static mxr_size_t s_iram_free_bytes MXR_IRAM_DATA_ATTR;
static mxr_size_t s_iram_min_free_bytes MXR_IRAM_DATA_ATTR;
static uint32_t s_iram_exec_allocs MXR_IRAM_DATA_ATTR;
static uint32_t s_iram_fallback_allocs MXR_IRAM_DATA_ATTR;
#endif

#define MXR_ACTIVE_TOTAL_REGIONS CONFIG_MXR_REGIONS

static mxr_region_t s_region[MXR_ACTIVE_TOTAL_REGIONS] MXR_IRAM_DATA_ATTR;


/* ================================================================
 *  Word-aligned memory helpers (IRAM-safe, no libc)
 * ================================================================ */
static inline void MXR_IRAM_ALLOC_ATTR mxr_memset4(void *ptr, size_t bytes)
{
    uint32_t *p = (uint32_t *)ptr;
    size_t words = bytes >> 2;
    for (size_t i = 0; i < words; i++)
    {
        p[i] = 0;
    }
}

static inline void MXR_IRAM_ALLOC_ATTR mxr_memcpy4(void *dst, const void *src, size_t bytes)
{
    uint32_t *d = (uint32_t *)dst;
    const uint32_t *s = (const uint32_t *)src;
    size_t words = bytes >> 2;
    for (size_t i = 0; i < words; i++)
    {
        d[i] = s[i];
    }
}

/* ================================================================
 *  Basic conversions (byte-based, relative offsets)
 * ================================================================ */
static inline void *MXR_IRAM_INLINE_ATTR mxr_off_to_ptr(mxr_offset_t off_bytes)
{
    return (void *)(s_arena_base + off_bytes);
}

static inline mxr_offset_t MXR_IRAM_INLINE_ATTR mxr_ptr_to_off(const void *ptr)
{
    return (mxr_offset_t)((const uint8_t *)ptr - s_arena_base);
}

#ifdef CONFIG_MXR_USE_IRAM
static inline void *MXR_IRAM_INLINE_ATTR mxr_iram_off_to_ptr(mxr_offset_t off_bytes)
{
    return (void *)(s_iram_base + off_bytes);
}

static inline mxr_offset_t MXR_IRAM_INLINE_ATTR mxr_iram_ptr_to_off(const void *ptr)
{
    return (mxr_offset_t)((const uint8_t *)ptr - s_iram_base);
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
    uintptr_t dram_end = dram_start + (uintptr_t)s_arena_total_bytes;

    if (p >= dram_start && p < dram_end)
    {
        if ((p & MXR_ALIGN_MASK) != 0)
        {
            return MXR_ARENA_NONE;
        }
        return MXR_ARENA_DRAM;
    }

#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        uintptr_t iram_start = (uintptr_t)s_iram_base;
        uintptr_t iram_end = iram_start + (uintptr_t)s_iram_total_bytes;

        if (p >= iram_start && p < iram_end)
        {
            if ((p & MXR_ALIGN_MASK) != 0)
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
 *  Descriptor table operations
 *
 *  Sorted by off_flags (uint32_t key).
 *  DRAM descriptors (bit31=0) first, IRAM (bit31=1) last.
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

/* Binary search by key. Used by free() and realloc(). */
static int MXR_IRAM_ATTR mxr_desc_find_key(uint32_t key)
{
    int left = 0;
    int right = (int)s_desc_count;

    while (left < right)
    {
        int mid = (left + right) / 2;
        uint32_t cur = s_desc[mid].off_flags;

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
    mxr_offset_t off_bytes,
    mxr_size_t len_bytes,
    bool iram,
    uint32_t len_flags)
{
    if (!s_initialized)
    {
        return false;
    }
    if (off_bytes > MXR_MAX_OFFSET_BYTES)
    {
        return false;
    }
    if (len_bytes == 0 || len_bytes > MXR_MAX_LEN_BYTES)
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
        if ((mxr_size_t)off_bytes + len_bytes > s_iram_total_bytes)
        {
            return false;
        }
#else
        return false;
#endif
    }
    else
    {
        if ((mxr_size_t)off_bytes + len_bytes > s_arena_total_bytes)
        {
            return false;
        }
    }

    if (s_desc_count >= CONFIG_MXR_MAX_DESC)
    {
        s_stats.alloc_fail_table_full++;
        return false;
    }

    uint32_t key = mxr_desc_make_key(off_bytes, iram);

    /* Binary search for insert position */
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

    /* Overlap checks (same arena only) */
    if (pos > 0 && mxr_desc_is_iram(&s_desc[pos - 1]) == iram)
    {
        mxr_offset_t prev_off = mxr_desc_off(&s_desc[pos - 1]);
        mxr_size_t prev_len = mxr_desc_len(&s_desc[pos - 1]);
        if ((mxr_size_t)prev_off + prev_len > (mxr_size_t)off_bytes)
        {
            return false;
        }
    }
    if (pos < s_desc_count && mxr_desc_is_iram(&s_desc[pos]) == iram)
    {
        mxr_offset_t next_off = mxr_desc_off(&s_desc[pos]);
        if ((mxr_size_t)off_bytes + len_bytes > (mxr_size_t)next_off)
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
        off_bytes,
        len_bytes,
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

static inline void MXR_IRAM_INLINE_ATTR mxr_desc_set_len(mxr_desc_t *d, uint32_t len_bytes)
{
    uint32_t flags = d->len_flags & MXR_LEN_FLAGS_MASK;
    d->len_flags = (len_bytes & MXR_LEN_MASK) | flags;
}

/* ================================================================
 *  DRAM region helpers
 * ================================================================ */
static int MXR_IRAM_ATTR mxr_region_by_off(mxr_offset_t off_bytes)
{
    for (uint8_t i = 0; i < s_region_count; i++)
    {
        mxr_offset_t start = s_region[i].start_byte;
        mxr_offset_t end = start + (mxr_offset_t)s_region[i].total_bytes;
        if (off_bytes >= start && off_bytes < end)
        {
            return i;
        }
    }
    return -1;
}

static int MXR_IRAM_ATTR mxr_region_for_size(mxr_size_t len_bytes, uint32_t caps)
{
    for (uint8_t i = 0; i < s_region_count; i++)
    {
        if (((uint32_t)s_region[i].caps & caps) != caps)
        {
            continue;
        }
        if (len_bytes < (mxr_size_t)s_region[i].min_bytes)
        {
            continue;
        }
        if (s_region[i].max_bytes != MXR_REGION_MAX_UNLIMITED)
        {
            if (len_bytes > (mxr_size_t)s_region[i].max_bytes)
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
    return ((uint32_t)s_region[region_index].caps & caps) == caps;
}

static bool MXR_IRAM_ATTR mxr_region_size_ok(int region_index, mxr_size_t bytes)
{
    if (region_index < 0 || region_index >= s_region_count)
    {
        return false;
    }
    if (bytes == 0)
    {
        return false;
    }
    if (bytes < (mxr_size_t)s_region[region_index].min_bytes)
    {
        return false;
    }
    if (s_region[region_index].max_bytes != MXR_REGION_MAX_UNLIMITED)
    {
        if (bytes > (mxr_size_t)s_region[region_index].max_bytes)
        {
            return false;
        }
    }
    return true;
}

/* Largest contiguous free block in a DRAM region (descriptor gap scan). */
static mxr_size_t MXR_IRAM_ATTR mxr_region_largest_free_bytes(uint8_t region_index)
{
    if (region_index >= s_region_count)
    {
        return 0;
    }

    mxr_offset_t region_start = s_region[region_index].start_byte;
    mxr_offset_t region_end = region_start + (mxr_offset_t)s_region[region_index].total_bytes;
    mxr_offset_t cur = region_start;
    mxr_size_t largest = 0;

    for (uint16_t i = 0; i < s_desc_count; i++)
    {
        if (mxr_desc_is_iram(&s_desc[i]))
        {
            break;
        }
        mxr_offset_t off = mxr_desc_off(&s_desc[i]);
        mxr_size_t len = mxr_desc_len(&s_desc[i]);
        mxr_offset_t block_end = off + (mxr_offset_t)len;

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
            mxr_size_t gap = (mxr_size_t)(off - cur);
            if (gap > largest)
            {
                largest = gap;
            }
        }
        if (block_end > cur)
        {
            cur = block_end;
        }
        if (cur >= region_end)
        {
            break;
        }
    }

    if (region_end > cur)
    {
        mxr_size_t gap = (mxr_size_t)(region_end - cur);
        if (gap > largest)
        {
            largest = gap;
        }
    }
    return largest;
}

static void MXR_IRAM_ATTR mxr_region_allocated(int region_index, mxr_size_t bytes)
{
    if (region_index >= 0 && region_index < s_region_count)
    {
        if (s_region[region_index].free_bytes >= bytes)
        {
            s_region[region_index].free_bytes -= bytes;
        }
        else
        {
            s_region[region_index].free_bytes = 0;
        }
        if (s_region[region_index].free_bytes < s_region[region_index].min_free_bytes)
        {
            s_region[region_index].min_free_bytes = s_region[region_index].free_bytes;
        }
    }

    if (s_stats.free_bytes >= (size_t)bytes)
    {
        s_stats.free_bytes -= (size_t)bytes;
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

static void MXR_IRAM_ATTR mxr_region_released(int region_index, mxr_size_t bytes)
{
    if (region_index >= 0 && region_index < s_region_count)
    {
        mxr_size_t new_free = s_region[region_index].free_bytes + bytes;
        if (new_free > s_region[region_index].total_bytes)
        {
            new_free = s_region[region_index].total_bytes;
        }
        s_region[region_index].free_bytes = new_free;
    }

    s_stats.free_bytes += (size_t)bytes;
    if (s_stats.free_bytes > s_stats.total_bytes)
    {
        s_stats.free_bytes = s_stats.total_bytes;
    }
}

/* ================================================================
 *  DRAM free-block search (descriptor gap scan)
 * ================================================================ */
static bool MXR_IRAM_ATTR mxr_find_free_from_start(
    int region_index,
    mxr_size_t bytes,
    mxr_offset_t *out_off)
{
    mxr_offset_t region_start = s_region[region_index].start_byte;
    mxr_offset_t region_end = region_start + (mxr_offset_t)s_region[region_index].total_bytes;
    mxr_offset_t cur = region_start;

    for (uint16_t i = 0; i < s_desc_count; i++)
    {
        if (mxr_desc_is_iram(&s_desc[i]))
        {
            break;
        }
        mxr_offset_t off = mxr_desc_off(&s_desc[i]);
        mxr_size_t len = mxr_desc_len(&s_desc[i]);
        mxr_offset_t block_end = off + (mxr_offset_t)len;

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
            mxr_size_t gap = (mxr_size_t)(off - cur);
            if (gap >= bytes)
            {
                *out_off = cur;
                return true;
            }
        }
        if (block_end > cur)
        {
            cur = block_end;
        }
        if (cur >= region_end)
        {
            break;
        }
    }

    if (region_end > cur)
    {
        mxr_size_t gap = (mxr_size_t)(region_end - cur);
        if (gap >= bytes)
        {
            *out_off = cur;
            return true;
        }
    }
    return false;
}

static bool MXR_IRAM_ATTR mxr_try_alloc_region(
    int region_index,
    mxr_size_t bytes,
    mxr_offset_t *out_off)
{
    if (region_index < 0 || region_index >= s_region_count)
    {
        return false;
    }
    if (bytes == 0 || bytes > MXR_MAX_LEN_BYTES)
    {
        return false;
    }
    if (s_region[region_index].free_bytes < bytes)
    {
        return false;
    }
    return mxr_find_free_from_start(region_index, bytes, out_off);
}

/* ================================================================
 *  IRAM helpers
 * ================================================================ */
#ifdef CONFIG_MXR_USE_IRAM

static void MXR_IRAM_ATTR mxr_iram_allocated(mxr_size_t bytes)
{
    if (s_iram_free_bytes >= bytes)
    {
        s_iram_free_bytes -= bytes;
    }
    else
    {
        s_iram_free_bytes = 0;
    }
    if (s_iram_free_bytes < s_iram_min_free_bytes)
    {
        s_iram_min_free_bytes = s_iram_free_bytes;
    }

    if (s_stats.free_bytes >= (size_t)bytes)
    {
        s_stats.free_bytes -= (size_t)bytes;
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

static void MXR_IRAM_ATTR mxr_iram_released(mxr_size_t bytes)
{
    mxr_size_t new_free = s_iram_free_bytes + bytes;
    if (new_free > s_iram_total_bytes)
    {
        new_free = s_iram_total_bytes;
    }
    s_iram_free_bytes = new_free;

    s_stats.free_bytes += (size_t)bytes;
    if (s_stats.free_bytes > s_stats.total_bytes)
    {
        s_stats.free_bytes = s_stats.total_bytes;
    }
}

static bool MXR_IRAM_ATTR mxr_iram_find_free_from_start(
    mxr_size_t bytes,
    mxr_offset_t *out_off)
{
    if (!s_iram_enabled)
    {
        return false;
    }
    if (bytes == 0 || bytes > s_iram_total_bytes)
    {
        return false;
    }
    if (s_iram_free_bytes < bytes)
    {
        return false;
    }

    mxr_offset_t cur = 0;
    mxr_offset_t end = s_iram_total_bytes;
    int first = mxr_desc_first_iram();

    for (int i = first; i < (int)s_desc_count; i++)
    {
        if (!mxr_desc_is_iram(&s_desc[i]))
        {
            break;
        }
        mxr_offset_t off = mxr_desc_off(&s_desc[i]);
        mxr_size_t len = mxr_desc_len(&s_desc[i]);
        mxr_offset_t block_end = off + (mxr_offset_t)len;

        if (off > cur)
        {
            mxr_size_t gap = (mxr_size_t)(off - cur);
            if (gap >= bytes)
            {
                *out_off = cur;
                return true;
            }
        }
        if (block_end > cur)
        {
            cur = block_end;
        }
        if (cur >= end)
        {
            break;
        }
    }

    if (end > cur)
    {
        mxr_size_t gap = (mxr_size_t)(end - cur);
        if (gap >= bytes)
        {
            *out_off = cur;
            return true;
        }
    }
    return false;
}

static bool MXR_IRAM_ATTR mxr_iram_find_free_from_end(
    mxr_size_t bytes,
    mxr_offset_t *out_off)
{
    if (!s_iram_enabled)
    {
        return false;
    }
    if (bytes == 0 || bytes > s_iram_total_bytes)
    {
        return false;
    }
    if (s_iram_free_bytes < bytes)
    {
        return false;
    }

    mxr_offset_t candidate_end = s_iram_total_bytes;

    for (int i = (int)s_desc_count - 1; i >= 0; i--)
    {
        if (!mxr_desc_is_iram(&s_desc[i]))
        {
            break;
        }
        mxr_offset_t off = mxr_desc_off(&s_desc[i]);
        mxr_size_t len = mxr_desc_len(&s_desc[i]);
        mxr_offset_t block_end = off + (mxr_offset_t)len;

        if (candidate_end > block_end)
        {
            mxr_size_t gap = (mxr_size_t)(candidate_end - block_end);
            if (gap >= bytes)
            {
                *out_off = candidate_end - (mxr_offset_t)bytes;
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
        if ((mxr_size_t)candidate_end >= bytes)
        {
            *out_off = candidate_end - (mxr_offset_t)bytes;
            return true;
        }
    }
    return false;
}

static mxr_size_t MXR_IRAM_ATTR mxr_iram_largest_free_bytes(void)
{
    if (!s_iram_enabled)
    {
        return 0;
    }

    mxr_offset_t cur = 0;
    mxr_offset_t end = s_iram_total_bytes;
    mxr_size_t largest = 0;
    int first = mxr_desc_first_iram();

    for (int i = first; i < (int)s_desc_count; i++)
    {
        if (!mxr_desc_is_iram(&s_desc[i]))
        {
            break;
        }
        mxr_offset_t off = mxr_desc_off(&s_desc[i]);
        mxr_size_t len = mxr_desc_len(&s_desc[i]);
        mxr_offset_t block_end = off + (mxr_offset_t)len;

        if (off > cur)
        {
            mxr_size_t gap = (mxr_size_t)(off - cur);
            if (gap > largest)
            {
                largest = gap;
            }
        }
        if (block_end > cur)
        {
            cur = block_end;
        }
        if (cur >= end)
        {
            break;
        }
    }

    if (end > cur)
    {
        mxr_size_t gap = (mxr_size_t)(end - cur);
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

static inline mxr_size_t MXR_IRAM_INLINE_ATTR mxr_iram_reserve_bytes(void)
{
    uint32_t reserve = CONFIG_MXR_IRAM_RESERVE_BYTES;
    return (mxr_size_t)mxr_align4(reserve);
}

static bool MXR_IRAM_ATTR mxr_iram_can_fallback(mxr_size_t bytes)
{
    if (!s_iram_enabled)
    {
        return false;
    }
    if (s_iram_free_bytes < bytes)
    {
        return false;
    }
    mxr_size_t need = bytes + mxr_iram_reserve_bytes();
    if (s_iram_free_bytes < need)
    {
        return false;
    }
    if (CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES != 0)
    {
        if (bytes > (mxr_size_t)CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES)
        {
            return false;
        }
    }
    return true;
}

static bool MXR_IRAM_ATTR mxr_iram_can_grow_fallback(
    mxr_size_t old_bytes,
    mxr_size_t new_bytes)
{
    if (!s_iram_enabled)
    {
        return false;
    }
    if (new_bytes <= old_bytes)
    {
        return true;
    }
    mxr_size_t extra = new_bytes - old_bytes;
    mxr_size_t reserve = mxr_iram_reserve_bytes();
    if (s_iram_free_bytes < extra + reserve)
    {
        return false;
    }
    if (CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES != 0)
    {
        if (new_bytes > (mxr_size_t)CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES)
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

    /*
     * _iram_end is AFTER .iram0.bss (which now holds s_desc[]).
     * IRAM heap starts after the descriptor table.
     */
    uint8_t *start = (uint8_t *)(((uint32_t)&_iram_end + 3) & ~3);
    uint8_t *end = (uint8_t *)(0x40100000 + CONFIG_SOC_IRAM_SIZE);

    s_iram_enabled = false;
    s_iram_base = NULL;
    s_iram_total_bytes = 0;
    s_iram_free_bytes = 0;
    s_iram_min_free_bytes = 0;
    s_iram_exec_allocs = 0;
    s_iram_fallback_allocs = 0;

    if (end <= start)
    {
        return;
    }

    size_t bytes = (size_t)(end - start);
    bytes &= ~(size_t)MXR_ALIGN_MASK;

    if (bytes <= 512 || bytes >= 0x00010000)
    {
        return;
    }

    s_iram_base = start;
    s_iram_total_bytes = (mxr_size_t)bytes;
    s_iram_free_bytes = (mxr_size_t)bytes;
    s_iram_min_free_bytes = (mxr_size_t)bytes;
    s_iram_enabled = true;

    ESP_EARLY_LOGI(TAG,
                   "IRAM heap ok: base=%p bytes=%u",
                   s_iram_base,
                   (unsigned)s_iram_total_bytes);
}

#endif /* CONFIG_MXR_USE_IRAM */

#ifdef CONFIG_MXR_CROSS_REGION_FALLBACK
static void *MXR_IRAM_ATTR mxr_try_cross_region(
    mxr_size_t bytes,
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
        mxr_size_t best_min = MXR_MAX_LEN_BYTES;

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
            if (s_region[i].free_bytes < bytes)
            {
                continue;
            }
#ifdef CONFIG_MXR_CROSS_REGION_CHECK_LARGEST
            if (mxr_region_largest_free_bytes(i) < bytes)
            {
                s_stats.cross_region_skip_fragmented++;
                tried |= (1u << i);
                continue;
            }
#endif
            if ((mxr_size_t)s_region[i].min_bytes < best_min)
            {
                best_min = (mxr_size_t)s_region[i].min_bytes;
                best = (int)i;
            }
        }

        if (best < 0)
        {
            return NULL;
        }

        tried |= (1u << best);

        mxr_offset_t off_bytes = 0;
        if (!mxr_try_alloc_region(best, bytes, &off_bytes))
        {
            continue;
        }
        if (!mxr_desc_insert_ex(off_bytes, bytes, false, 0))
        {
            return NULL;
        }

        s_region[best].alloc_count++;
        mxr_region_allocated(best, bytes);
        s_stats.cross_region_allocs++;
        return mxr_off_to_ptr(off_bytes);
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

    size = mxr_align4(size);

    if (size > MXR_MAX_LEN_BYTES)
    {
        s_stats.alloc_fail_no_memory++;
        return NULL;
    }

    mxr_size_t bytes = (mxr_size_t)size;

#ifdef CONFIG_MXR_USE_IRAM
    /* EXEC allocations go only to IRAM */
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

        mxr_offset_t off_bytes = 0;
        if (!mxr_iram_find_free_from_start(bytes, &off_bytes))
        {
            s_stats.alloc_fail_no_memory++;
            return NULL;
        }
        if (!mxr_desc_insert_ex(off_bytes, bytes, true, MXR_LEN_FLAG_EXEC))
        {
            return NULL;
        }

        mxr_iram_allocated(bytes);
        s_iram_exec_allocs++;
        s_stats.exec_allocs++;
        return mxr_iram_off_to_ptr(off_bytes);
    }
#else
    if (caps & MALLOC_CAP_EXEC)
    {
        s_stats.alloc_fail_no_memory++;
        return NULL;
    }
#endif

#ifdef CONFIG_MXR_USE_IRAM
    /*
     * IRAM-first for pure MALLOC_CAP_32BIT.
     *
     * Matches original SDK behavior: IRAM region was checked
     * BEFORE DRAM. malloc() / calloc() / zalloc() / realloc()
     * all use MALLOC_CAP_32BIT, so their allocations land in
     * IRAM first, freeing DRAM for 8BIT / DMA consumers
     * (i2c, i2s, spi, pwm, adc, wifi TX, lwIP pbuf).
     *
     * Guards:
     *   - mxr_caps_allow_iram_fallback: blocks 8BIT / DMA / SPIRAM / EXEC
     *   - mxr_iram_can_fallback: respects IRAM_RESERVE_BYTES and
     *     IRAM_FALLBACK_MAX_BYTES
     */
    if (s_iram_enabled &&
        mxr_caps_allow_iram_fallback(caps) &&
        mxr_iram_can_fallback(bytes))
    {
        mxr_offset_t off_bytes = 0;
        if (mxr_iram_find_free_from_end(bytes, &off_bytes))
        {
            if (mxr_desc_insert_ex(off_bytes, bytes, true, 0))
            {
                mxr_iram_allocated(bytes);
                s_iram_fallback_allocs++;
                s_stats.iram_fallback_allocs++;
                return mxr_iram_off_to_ptr(off_bytes);
            }
        }
    }
#endif

    /* DRAM allocation — Step 1: own size-class region */
    int region = mxr_region_for_size(bytes, caps);

    if (region >= 0)
    {
        mxr_offset_t off_bytes = 0;
        if (mxr_try_alloc_region(region, bytes, &off_bytes))
        {
            if (!mxr_desc_insert_ex(off_bytes, bytes, false, 0))
            {
                return NULL;
            }
            s_region[region].alloc_count++;
            mxr_region_allocated(region, bytes);
            return mxr_off_to_ptr(off_bytes);
        }
    }

    /* Step 2: cross-region DRAM fallback (last resort) */
#ifdef CONFIG_MXR_CROSS_REGION_FALLBACK
    {
        void *fallback_ptr = mxr_try_cross_region(bytes, caps, region);
        if (fallback_ptr)
        {
            return fallback_ptr;
        }
    }
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
        mxr_offset_t off_bytes = mxr_ptr_to_off(ptr);
        uint32_t key = mxr_desc_make_key(off_bytes, false);

        /* Binary search */
        int index = mxr_desc_find_key(key);
        if (index < 0)
        {
            s_stats.invalid_free_attempts++;
            return;
        }

        mxr_size_t len_bytes = mxr_desc_len(&s_desc[index]);
        int region = mxr_region_by_off(off_bytes);

        mxr_desc_remove(index);

        if (region >= 0 && s_region[region].alloc_count > 0)
        {
            s_region[region].alloc_count--;
        }
        mxr_region_released(region, len_bytes);
        return;
    }

#ifdef CONFIG_MXR_USE_IRAM
    if (arena == MXR_ARENA_IRAM)
    {
        mxr_offset_t off_bytes = mxr_iram_ptr_to_off(ptr);
        uint32_t key = mxr_desc_make_key(off_bytes, true);

        int index = mxr_desc_find_key(key);
        if (index < 0)
        {
            s_stats.invalid_free_attempts++;
            return;
        }

        mxr_size_t len_bytes = mxr_desc_len(&s_desc[index]);
        mxr_desc_remove(index);
        mxr_iram_released(len_bytes);
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
        size_t clear_bytes = mxr_align4(total_bytes ? total_bytes : 1);
        mxr_memset4(ptr, clear_bytes);
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
        size_t clear_bytes = mxr_align4(size ? size : 1);
        mxr_memset4(ptr, clear_bytes);
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

    newsize = mxr_align4(newsize);

    if (newsize > MXR_MAX_LEN_BYTES)
    {
        return NULL;
    }

    mxr_size_t new_bytes = (mxr_size_t)newsize;
    if (new_bytes == 0)
    {
        new_bytes = MXR_ALIGN_SIZE;
    }

    mxr_lock();

    mxr_arena_id_t arena = mxr_ptr_to_arena(ptr);
    if (arena == MXR_ARENA_NONE)
    {
        s_stats.invalid_free_attempts++;
        mxr_unlock();
        return NULL;
    }

    /* ---- DRAM realloc ---- */
    if (arena == MXR_ARENA_DRAM)
    {
        mxr_offset_t off_bytes = mxr_ptr_to_off(ptr);
        uint32_t key = mxr_desc_make_key(off_bytes, false);
        int index = mxr_desc_find_key(key);

        if (index < 0)
        {
            s_stats.invalid_free_attempts++;
            mxr_unlock();
            return NULL;
        }

        mxr_size_t old_bytes = mxr_desc_len(&s_desc[index]);
        int region = mxr_region_by_off(off_bytes);
        bool caps_ok = mxr_region_caps_ok(region, caps);
        bool in_place_allowed = caps_ok && mxr_region_size_ok(region, new_bytes);

        /* Same size */
        if (new_bytes == old_bytes && region >= 0 && in_place_allowed)
        {
            mxr_unlock();
            return ptr;
        }

        /* Shrink in place */
        if (new_bytes < old_bytes && region >= 0 && in_place_allowed)
        {
            mxr_size_t diff = old_bytes - new_bytes;
            mxr_desc_set_len(&s_desc[index], (uint32_t)new_bytes);
            mxr_region_released(region, diff);
            mxr_unlock();
            return ptr;
        }

        /* Grow in place */
        if (new_bytes > old_bytes && region >= 0 && in_place_allowed)
        {
            mxr_size_t extra = new_bytes - old_bytes;
            mxr_offset_t block_end = off_bytes + (mxr_offset_t)old_bytes;
            mxr_offset_t region_end =
                s_region[region].start_byte + (mxr_offset_t)s_region[region].total_bytes;

            mxr_offset_t next_boundary;
            if (index + 1 < s_desc_count &&
                !mxr_desc_is_iram(&s_desc[index + 1]))
            {
                mxr_offset_t next_off = mxr_desc_off(&s_desc[index + 1]);
                next_boundary = (next_off < region_end) ? next_off : region_end;
            }
            else
            {
                next_boundary = region_end;
            }

            if (next_boundary >= block_end)
            {
                mxr_size_t gap = (mxr_size_t)(next_boundary - block_end);
                if (gap >= extra)
                {
                    mxr_desc_set_len(&s_desc[index], (uint32_t)new_bytes);
                    mxr_region_allocated(region, extra);
                    mxr_unlock();
                    return ptr;
                }
            }
        }

        /* Move */
        mxr_size_t copy_bytes = (old_bytes < new_bytes) ? old_bytes : new_bytes;
        void *new_ptr = mxr_malloc_caps_locked(newsize, caps);
        if (!new_ptr)
        {
            mxr_unlock();
            return NULL;
        }

        mxr_unlock();
        if (new_ptr != ptr)
        {
            mxr_memcpy4(new_ptr, ptr, (size_t)copy_bytes);
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
    /* ---- IRAM realloc ---- */
    if (arena == MXR_ARENA_IRAM)
    {
        mxr_offset_t off_bytes = mxr_iram_ptr_to_off(ptr);
        uint32_t key = mxr_desc_make_key(off_bytes, true);
        int index = mxr_desc_find_key(key);

        if (index < 0)
        {
            s_stats.invalid_free_attempts++;
            mxr_unlock();
            return NULL;
        }

        bool old_exec = mxr_desc_is_exec(&s_desc[index]);

        if (old_exec && !(caps & MALLOC_CAP_EXEC))
        {
            caps |= MALLOC_CAP_EXEC;
        }

        mxr_size_t old_bytes = mxr_desc_len(&s_desc[index]);
        bool want_exec = (caps & MALLOC_CAP_EXEC) != 0;
        bool in_place_allowed = false;

        if (want_exec)
        {
            in_place_allowed =
                old_exec &&
                ((caps & ~(MALLOC_CAP_EXEC | MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)) == 0);
        }
        else
        {
            in_place_allowed =
                !old_exec &&
                mxr_caps_allow_iram_fallback(caps);
        }

        if (in_place_allowed)
        {
            /* Same size */
            if (new_bytes == old_bytes)
            {
                mxr_unlock();
                return ptr;
            }

            /* Shrink */
            if (new_bytes < old_bytes)
            {
                mxr_size_t diff = old_bytes - new_bytes;
                mxr_desc_set_len(&s_desc[index], (uint32_t)new_bytes);
                mxr_iram_released(diff);
                mxr_unlock();
                return ptr;
            }

            /* Grow */
            mxr_size_t extra = new_bytes - old_bytes;

            if (!old_exec)
            {
                if (!mxr_iram_can_grow_fallback(old_bytes, new_bytes))
                {
                    in_place_allowed = false;
                }
            }

            if (in_place_allowed)
            {
                mxr_offset_t block_end = off_bytes + (mxr_offset_t)old_bytes;
                mxr_offset_t next_boundary;

                if (index + 1 < s_desc_count &&
                    mxr_desc_is_iram(&s_desc[index + 1]))
                {
                    next_boundary = mxr_desc_off(&s_desc[index + 1]);
                }
                else
                {
                    next_boundary = s_iram_total_bytes;
                }

                if (next_boundary >= block_end)
                {
                    mxr_size_t gap = (mxr_size_t)(next_boundary - block_end);
                    if (gap >= extra)
                    {
                        mxr_desc_set_len(&s_desc[index], (uint32_t)new_bytes);
                        mxr_iram_allocated(extra);
                        mxr_unlock();
                        return ptr;
                    }
                }
            }
        }

        /* Move */
        mxr_size_t copy_bytes = (old_bytes < new_bytes) ? old_bytes : new_bytes;
        void *new_ptr = mxr_malloc_caps_locked(newsize, caps);
        if (!new_ptr)
        {
            mxr_unlock();
            return NULL;
        }

        mxr_unlock();
        if (new_ptr != ptr)
        {
            mxr_memcpy4(new_ptr, ptr, (size_t)copy_bytes);
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
    s_region[0].caps = (mxr_caps_t)MXR_DRAM_CAPS_DEFAULT;
    s_region[0].start_byte = 0;
    s_region[0].total_bytes = s_arena_total_bytes;
    s_region[0].min_bytes = (mxr_class_t)MXR_ALIGN_SIZE;
    s_region[0].max_bytes = MXR_REGION_MAX_UNLIMITED;
    s_region[0].free_bytes = s_arena_total_bytes;
    s_region[0].min_free_bytes = s_arena_total_bytes;
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
        ESP_EARLY_LOGE(TAG, "region percent sum must be <= 100, got %u",
                       (unsigned)percent_sum);
        return false;
    }

    /* Sanity check size classes */
    mxr_size_t expected_min = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        mxr_class_t min_b = cfg[i].min_bytes;
        mxr_class_t max_b = cfg[i].max_bytes;

        if (min_b == 0)
        {
            min_b = (mxr_class_t)MXR_ALIGN_SIZE;
        }
#ifndef CONFIG_MXR_COMPACT_TYPES
        if ((mxr_size_t)min_b > MXR_MAX_LEN_BYTES)
        {
            ESP_EARLY_LOGE(TAG, "region %u min too large: %u",
                           (unsigned)i, (unsigned)min_b);
            return false;
        }
#endif
        if (max_b == MXR_REGION_MAX_UNLIMITED && i != (uint8_t)(count - 1))
        {
            ESP_EARLY_LOGE(TAG, "only last region may be unlimited: region %u",
                           (unsigned)i);
            return false;
        }
        if (max_b != MXR_REGION_MAX_UNLIMITED)
        {
#ifndef CONFIG_MXR_COMPACT_TYPES
            if ((mxr_size_t)max_b > MXR_MAX_LEN_BYTES)
            {
                max_b = (mxr_class_t)MXR_MAX_LEN_BYTES;
            }
#endif
            if (min_b > max_b)
            {
                ESP_EARLY_LOGE(TAG, "region %u bad min/max: %u/%u",
                               (unsigned)i, (unsigned)min_b, (unsigned)max_b);
                return false;
            }
        }
        if (i > 0)
        {
            if ((mxr_size_t)min_b < expected_min)
            {
                ESP_EARLY_LOGE(TAG, "region %u overlaps previous: min=%u expected=%u",
                               (unsigned)i, (unsigned)min_b, (unsigned)expected_min);
                return false;
            }
            if ((mxr_size_t)min_b > expected_min)
            {
                ESP_EARLY_LOGE(TAG, "gap before region %u: expected=%u min=%u",
                               (unsigned)i, (unsigned)expected_min, (unsigned)min_b);
                return false;
            }
        }
        if (max_b == MXR_REGION_MAX_UNLIMITED
#ifndef CONFIG_MXR_COMPACT_TYPES
            || (mxr_size_t)max_b >= MXR_MAX_LEN_BYTES
#endif
        )
        {
            expected_min = MXR_MAX_LEN_BYTES;
        }
        else
        {
            expected_min = (mxr_size_t)max_b + 1;
        }
    }

    /* Allocate memory to regions */
    mxr_size_t remaining_bytes = s_arena_total_bytes;

    for (uint8_t i = 0; i < count; i++)
    {
        mxr_class_t min_b = cfg[i].min_bytes;
        mxr_class_t max_b = cfg[i].max_bytes;

        if (min_b == 0)
        {
            min_b = (mxr_class_t)MXR_ALIGN_SIZE;
        }
#ifndef CONFIG_MXR_COMPACT_TYPES
        if (max_b != MXR_REGION_MAX_UNLIMITED && (mxr_size_t)max_b > MXR_MAX_LEN_BYTES)
        {
            max_b = (mxr_class_t)MXR_MAX_LEN_BYTES;
        }
#endif

        mxr_size_t bytes;
        if (i == (uint8_t)(count - 1) && cfg[i].percent == 0)
        {
            bytes = remaining_bytes;
        }
        else
        {
            bytes = (mxr_size_t)(((uint64_t)s_arena_total_bytes * cfg[i].percent) / 100);
            bytes = (mxr_size_t)mxr_align4((size_t)bytes);
        }

        if (bytes < (mxr_size_t)min_b)
        {
            bytes = (mxr_size_t)min_b;
        }
        if (bytes > remaining_bytes)
        {
            ESP_EARLY_LOGE(TAG, "region %u too large: %u > %u",
                           (unsigned)i, (unsigned)bytes, (unsigned)remaining_bytes);
            return false;
        }

        s_region[s_region_count].caps = (mxr_caps_t)MXR_DRAM_CAPS_DEFAULT;
        s_region[s_region_count].start_byte = (mxr_offset_t)(s_arena_total_bytes - remaining_bytes);
        s_region[s_region_count].total_bytes = bytes;
        s_region[s_region_count].min_bytes = min_b;
        s_region[s_region_count].max_bytes = max_b;
        s_region[s_region_count].free_bytes = bytes;
        s_region[s_region_count].min_free_bytes = bytes;
        s_region[s_region_count].alloc_count = 0;

        remaining_bytes -= bytes;
        s_region_count++;
    }

    /* Add leftover to last region */
    if (remaining_bytes > 0)
    {
        s_region[count - 1].total_bytes += remaining_bytes;
        s_region[count - 1].free_bytes = s_region[count - 1].total_bytes;
        s_region[count - 1].min_free_bytes = s_region[count - 1].free_bytes;
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
    uint32_t *out_bytes,
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
            if (value > 0x7FFFFFFF)
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

        out_bytes[count] = value;
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
    uint32_t boundary_bytes[MXR_ACTIVE_TOTAL_REGIONS];
    uint8_t percent_count = 0;
    uint8_t boundary_count = 0;

    const char *percent_end =
        mxr_parse_percent(CONFIG_MXR_REGION_PERCENTS, cfg, total, &percent_count);
    const char *boundary_end =
        mxr_parse_boundaries(CONFIG_MXR_REGION_SIZES, boundary_bytes, total, &boundary_count);

    if (percent_count != total)
    {
        ESP_EARLY_LOGW(TAG, "percent count mismatch: got %u, expected %u",
                       (unsigned)percent_count, (unsigned)total);
        return false;
    }
    if (boundary_count != total)
    {
        ESP_EARLY_LOGW(TAG, "boundary count mismatch: got %u, expected %u",
                       (unsigned)boundary_count, (unsigned)total);
        return false;
    }
    if (mxr_has_extra_csv(percent_end) || mxr_has_extra_csv(boundary_end))
    {
        ESP_EARLY_LOGW(TAG, "extra CSV values detected: expected %u entries",
                       (unsigned)total);
        return false;
    }

    /* Align boundaries to 4 bytes */
    for (uint8_t i = 0; i < total; i++)
    {
        uint32_t b = (uint32_t)mxr_align4(boundary_bytes[i]);
        if (b < MXR_ALIGN_SIZE)
        {
            b = MXR_ALIGN_SIZE;
        }
        boundary_bytes[i] = b;
    }

    /* Boundaries must be strictly increasing */
    for (uint8_t i = 1; i < total; i++)
    {
        if (boundary_bytes[i] <= boundary_bytes[i - 1])
        {
            ESP_EARLY_LOGE(TAG,
                           "boundaries must be strictly increasing: "
                           "b[%u]=%u, b[%u]=%u",
                           (unsigned)(i - 1), (unsigned)boundary_bytes[i - 1],
                           (unsigned)i, (unsigned)boundary_bytes[i]);
            return false;
        }
    }

    /* Build region config */
    for (uint8_t i = 0; i < total; i++)
    {
#ifdef CONFIG_MXR_COMPACT_TYPES
        if (boundary_bytes[i] > 0xFFFF)
        {
            ESP_EARLY_LOGE(TAG, "boundary[%u]=%u exceeds compact max 65535",
                           (unsigned)i, (unsigned)boundary_bytes[i]);
            return false;
        }
        if (i < (uint8_t)(total - 1) && boundary_bytes[i + 1] - 1 > 0xFFFF)
        {
            ESP_EARLY_LOGE(TAG, "boundary[%u]-1=%u exceeds compact max 65535",
                           (unsigned)(i + 1), (unsigned)(boundary_bytes[i + 1] - 1));
            return false;
        }
#endif
        cfg[i].min_bytes = (mxr_class_t)boundary_bytes[i];
        if (i == (uint8_t)(total - 1))
        {
            cfg[i].max_bytes = MXR_REGION_MAX_UNLIMITED;
        }
        else
        {
            cfg[i].max_bytes = (mxr_class_t)(boundary_bytes[i + 1] - 1);
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
    bytes &= ~(size_t)MXR_ALIGN_MASK;

    if (bytes > MXR_MAX_ARENA_BYTES)
    {
        ESP_EARLY_LOGE(TAG, "arena too large: %u bytes", (unsigned)bytes);
        return;
    }

    s_arena_base = start;
    s_arena_total_bytes = (mxr_size_t)bytes;

    /* No bitmap — descriptor gap search only */

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


    mxr_size_t largest_bytes = 0;
    for (uint8_t i = 0; i < s_region_count; i++)
    {
        if (s_region[i].total_bytes > largest_bytes)
        {
            largest_bytes = s_region[i].total_bytes;
        }
    }

    size_t total_bytes = s_arena_total_bytes;
    size_t free_bytes = s_arena_total_bytes;

    s_stats.iram_total_bytes = 0;
    s_stats.iram_free_bytes = 0;
    s_stats.iram_min_free_bytes = 0;
    s_stats.exec_allocs = 0;
    s_stats.iram_fallback_allocs = 0;

#ifdef CONFIG_MXR_USE_IRAM
    if (s_iram_enabled)
    {
        total_bytes += s_iram_total_bytes;
        free_bytes += s_iram_total_bytes;

        s_stats.iram_total_bytes = s_iram_total_bytes;
        s_stats.iram_free_bytes = s_iram_total_bytes;
        s_stats.iram_min_free_bytes = s_iram_total_bytes;

        mxr_size_t iram_largest = mxr_iram_largest_free_bytes();
        if (iram_largest > largest_bytes)
        {
            largest_bytes = iram_largest;
        }
    }
#endif

    s_stats.initialized = true;
    s_stats.region_count = s_region_count;
    s_stats.total_bytes = total_bytes;
    s_stats.free_bytes = free_bytes;
    s_stats.min_free_bytes = total_bytes;
    s_stats.largest_free_block_bytes = (size_t)largest_bytes;

    s_initialized = true;

#if defined(CONFIG_MXR_DESC_IN_IRAM_TEXT)
    ESP_EARLY_LOGI(TAG,
                   "init ok: base=%p bytes=%u desc=IRAM(.iram0.text) %u bytes",
                   s_arena_base,
                   (unsigned)s_arena_total_bytes,
                   (unsigned)(CONFIG_MXR_MAX_DESC * sizeof(mxr_desc_t)));
#elif defined(CONFIG_MXR_DESC_IN_IRAM_BSS)
    ESP_EARLY_LOGI(TAG,
                   "init ok: base=%p bytes=%u desc=IRAM(.iram0.bss) %u bytes",
                   s_arena_base,
                   (unsigned)s_arena_total_bytes,
                   (unsigned)(CONFIG_MXR_MAX_DESC * sizeof(mxr_desc_t)));
#else
    ESP_EARLY_LOGI(TAG,
                   "init ok: base=%p bytes=%u desc=DRAM(.bss) %u bytes",
                   s_arena_base,
                   (unsigned)s_arena_total_bytes,
                   (unsigned)(CONFIG_MXR_MAX_DESC * sizeof(mxr_desc_t)));
#endif
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
    mxr_size_t largest_bytes = 0;

    for (uint8_t i = 0; i < s_region_count; i++)
    {
        total_bytes += (size_t)s_region[i].total_bytes;
        free_bytes += (size_t)s_region[i].free_bytes;

        mxr_size_t lr = mxr_region_largest_free_bytes(i);
        if (lr > largest_bytes)
        {
            largest_bytes = lr;
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
        total_bytes += s_iram_total_bytes;
        free_bytes += s_iram_free_bytes;

        s_stats.iram_total_bytes = s_iram_total_bytes;
        s_stats.iram_free_bytes = s_iram_free_bytes;
        s_stats.iram_min_free_bytes = s_iram_min_free_bytes;
        s_stats.exec_allocs = s_iram_exec_allocs;
        s_stats.iram_fallback_allocs = s_iram_fallback_allocs;

        mxr_size_t il = mxr_iram_largest_free_bytes();
        if (il > largest_bytes)
        {
            largest_bytes = il;
        }
    }
#endif

    s_stats.total_bytes = total_bytes;
    s_stats.free_bytes = free_bytes;
    s_stats.largest_free_block_bytes = (size_t)largest_bytes;

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
    status->start_byte = s_region[i].start_byte;
    status->total_bytes = s_region[i].total_bytes;
    status->min_bytes = s_region[i].min_bytes;
    status->max_bytes = s_region[i].max_bytes;
    status->free_bytes = s_region[i].free_bytes;
    status->min_free_bytes = s_region[i].min_free_bytes;
    status->largest_free_bytes = mxr_region_largest_free_bytes(i);
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
            bytes += (size_t)s_region[i].free_bytes;
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
                bytes += (size_t)s_iram_min_free_bytes;
            }
            else
            {
                mxr_size_t reserve = mxr_iram_reserve_bytes();
                if (s_iram_min_free_bytes > reserve)
                {
                    bytes += (size_t)(s_iram_min_free_bytes - reserve);
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
            bytes += (size_t)s_region[i].min_free_bytes;
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
                bytes += (size_t)s_iram_min_free_bytes;
            }
            else
            {
                mxr_size_t reserve = mxr_iram_reserve_bytes();
                if (s_iram_min_free_bytes > reserve)
                {
                    bytes += (size_t)(s_iram_min_free_bytes - reserve);
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

#if defined(CONFIG_MXR_DUMP_NORMAL) || defined(CONFIG_MXR_DUMP_FULL)

    ESP_EARLY_LOGI(TAG, "search mode: descriptor (bytes)");

#if defined(CONFIG_MXR_DESC_IN_IRAM_TEXT)
    ESP_EARLY_LOGI(TAG,
                   "desc used=%u/%u max_used=%u desc=IRAM(.text) %u bytes",
#elif defined(CONFIG_MXR_DESC_IN_IRAM_BSS)
    ESP_EARLY_LOGI(TAG,
                   "desc used=%u/%u max_used=%u desc=IRAM(.bss) %u bytes",
#else
    ESP_EARLY_LOGI(TAG,
                   "desc used=%u/%u max_used=%u desc=DRAM %u bytes",
#endif
                   (unsigned)st.active_allocs,
                   (unsigned)st.desc_capacity,
                   (unsigned)st.max_active_allocs,
                   (unsigned)(CONFIG_MXR_MAX_DESC * sizeof(mxr_desc_t)));

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
                       (unsigned)s_iram_total_bytes,
                       (unsigned)s_iram_free_bytes,
                       (unsigned)s_iram_min_free_bytes);
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
                       (unsigned)rs.start_byte,
                       (unsigned)rs.total_bytes,
                       (unsigned)rs.min_bytes,
                       (unsigned)rs.max_bytes,
                       (unsigned)rs.free_bytes,
                       (unsigned)rs.min_free_bytes,
                       (unsigned)rs.largest_free_bytes,
                       (unsigned)rs.alloc_count);
    }

    ESP_EARLY_LOGI(TAG,
                   "stats: fail_mem=%u fail_table=%u invalid_free=%u max_allocs=%u",
                   (unsigned)st.alloc_fail_no_memory,
                   (unsigned)st.alloc_fail_table_full,
                   (unsigned)st.invalid_free_attempts,
                   (unsigned)st.max_active_allocs);

#endif /* NORMAL || FULL */

#if defined(CONFIG_MXR_DUMP_FULL)
    {
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
    }
#endif /* FULL */
}