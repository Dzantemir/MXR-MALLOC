#pragma once

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#else
#include "sdkconfig.h"
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef MXR_IRAM_INLINE_ATTR
#define MXR_IRAM_INLINE_ATTR
#endif

#ifndef MXR_REALLOC_ZERO_FREES
#define MXR_REALLOC_ZERO_FREES 1
#endif

    /* ================================================================
     *  MxR-malloc v3 for ESP8266 RTOS SDK
     * ================================================================ */

#ifndef CONFIG_MXR_MAX_DESC
#define CONFIG_MXR_MAX_DESC 250
#endif

#ifndef CONFIG_MXR_IRAM_MAX_DESC
#define CONFIG_MXR_IRAM_MAX_DESC 150
#endif

#define MXR_REGIONS_MAX 32
#define MXR_REGIONS_MIN 1

#ifndef MXR_PARSED_REGION_COUNT
#define MXR_PARSED_REGION_COUNT 1
#endif

#if MXR_PARSED_REGION_COUNT > MXR_REGIONS_MAX
#undef MXR_PARSED_REGION_COUNT
#define MXR_PARSED_REGION_COUNT MXR_REGIONS_MAX
#elif MXR_PARSED_REGION_COUNT < MXR_REGIONS_MIN
#undef MXR_PARSED_REGION_COUNT
#define MXR_PARSED_REGION_COUNT MXR_REGIONS_MIN
#endif

#define MXR_USER_REGIONS MXR_PARSED_REGION_COUNT



#ifndef MXR_IRAM_FB_PARSED_REGION_COUNT
#define MXR_IRAM_FB_PARSED_REGION_COUNT 1
#endif

#define MXR_IRAM_FB_REGIONS_MAX 32
#define MXR_IRAM_FB_REGIONS_MIN 1

#if MXR_IRAM_FB_PARSED_REGION_COUNT > MXR_IRAM_FB_REGIONS_MAX
#undef MXR_IRAM_FB_PARSED_REGION_COUNT
#define MXR_IRAM_FB_PARSED_REGION_COUNT MXR_IRAM_FB_REGIONS_MAX

#elif MXR_IRAM_FB_PARSED_REGION_COUNT < MXR_IRAM_FB_REGIONS_MIN
#undef MXR_IRAM_FB_PARSED_REGION_COUNT
#define MXR_IRAM_FB_PARSED_REGION_COUNT MXR_IRAM_FB_REGIONS_MIN
#endif

#define MXR_IRAM_FB_REGION_COUNT MXR_IRAM_FB_PARSED_REGION_COUNT

#define MXR_ALIGN_SIZE 4
#define MXR_ALIGN_MASK (MXR_ALIGN_SIZE - 1)

#define MXR_OFF_BITS 31
#define MXR_LEN_BITS 31
#define MXR_OFF_MASK ((uint32_t)((1u << MXR_OFF_BITS) - 1u))
#define MXR_LEN_MASK ((uint32_t)((1u << MXR_LEN_BITS) - 1u))
#define MXR_OFF_FLAGS_MASK ((uint32_t)~MXR_OFF_MASK)
#define MXR_LEN_FLAGS_MASK ((uint32_t)~MXR_LEN_MASK)
#define MXR_LEN_FLAG_EXEC ((uint32_t)(1u << 31))
#define MXR_MAX_OFFSET_BYTES MXR_OFF_MASK
#define MXR_MAX_LEN_BYTES ((size_t)MXR_LEN_MASK)
#define MXR_MAX_ARENA_BYTES MXR_MAX_LEN_BYTES
#define MXR_REGION_MAX_UNLIMITED 0

/* ================================================================
 *  Platform-dependent type widths
 * ================================================================ */
#ifdef CONFIG_MXR_COMPACT_TYPES
    typedef uint16_t mxr_caps_t;
    typedef uint16_t mxr_class_t;
    typedef uint16_t mxr_count_t;
#else
typedef uint32_t mxr_caps_t;
typedef uint32_t mxr_class_t;
typedef uint32_t mxr_count_t;
#endif

/* ================================================================
 *  Capability bits
 * ================================================================ */
#ifndef MALLOC_CAP_EXEC
#define MALLOC_CAP_EXEC (1 << 0)
#endif
#ifndef MALLOC_CAP_32BIT
#define MALLOC_CAP_32BIT (1 << 1)
#endif
#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT (1 << 2)
#endif
#ifndef MALLOC_CAP_DMA
#define MALLOC_CAP_DMA (1 << 3)
#endif
#ifndef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM (1 << 10)
#endif
#ifndef MALLOC_CAP_INTERNAL
#define MALLOC_CAP_INTERNAL (1 << 11)
#endif

#define MXR_DRAM_CAPS_DEFAULT \
    (MALLOC_CAP_8BIT | MALLOC_CAP_32BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
#define MXR_IRAM_CAPS_DEFAULT \
    (MALLOC_CAP_32BIT | MALLOC_CAP_EXEC)
#define MXR_IRAM_FB_CAPS_DEFAULT \
    (MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)

/* ================================================================
 *  Descriptor table placement attribute
 * ================================================================ */
#if defined(CONFIG_MXR_DESC_IN_IRAM_TEXT)
#define MXR_IRAM_DATA_ATTR __attribute__((section(".iram0.text"), aligned(4)))
#elif defined(CONFIG_MXR_DESC_IN_IRAM_BSS)
#define MXR_IRAM_DATA_ATTR __attribute__((section(".iram0.bss"), aligned(4)))
#else
#define MXR_IRAM_DATA_ATTR
#endif

    /* ================================================================
     *  Allocation descriptor — always 8 bytes
     * ================================================================ */
    typedef struct
    {
        uint32_t off_flags;
        uint32_t len_flags;
    } mxr_desc_t;

    _Static_assert(sizeof(mxr_desc_t) == 8, "desc must be 8 bytes");

    /* ================================================================
     *  Region configuration (build-time)
     * ================================================================ */
    typedef struct
    {
        uint8_t percent;
        mxr_class_t min_bytes;
        mxr_class_t max_bytes;
    } mxr_region_cfg_t;

    /* ================================================================
     *  Runtime region state (DRAM + IRAM fallback)
     * ================================================================ */
    typedef struct
    {
        mxr_caps_t caps;
        uint32_t start_byte;
        uint32_t total_bytes;
        mxr_class_t min_bytes;
        mxr_class_t max_bytes;
        uint32_t free_bytes;
        uint32_t min_free_bytes;
        mxr_count_t alloc_count;
    } mxr_region_t;

    /* ================================================================
     *  Region status for diagnostics
     * ================================================================ */
    typedef struct
    {
        mxr_caps_t caps;
        uint32_t start_byte;
        uint32_t total_bytes;
        mxr_class_t min_bytes;
        mxr_class_t max_bytes;
        uint32_t free_bytes;
        uint32_t min_free_bytes;
        uint32_t largest_free_bytes;
        mxr_count_t alloc_count;
    } mxr_region_status_t;

    /* ================================================================
     *  Global allocator status
     * ================================================================ */
    typedef struct
    {
        bool initialized;
        uint8_t region_count;
        uint8_t iram_fb_region_count;
        uint16_t dram_desc_capacity;
        uint16_t iram_desc_capacity;
        uint16_t dram_active_allocs;
        uint16_t iram_active_allocs;
        uint16_t max_active_allocs;
        size_t total_bytes;
        size_t free_bytes;
        size_t min_free_bytes;
        size_t largest_free_block_bytes;
        size_t iram_total_bytes;
        size_t iram_free_bytes;
        size_t iram_min_free_bytes;
        size_t iram_fb_zone_total_bytes;
        uint32_t exec_allocs;
        uint32_t iram_fallback_allocs;
        uint32_t cross_region_allocs;
        uint32_t alloc_fail_no_memory;
        uint32_t alloc_fail_table_full;
        uint32_t invalid_free_attempts;
        uint32_t cross_region_skip_fragmented;
    } mxr_status_t;

    /* ДОБАВЛЕНО: проверка кратности 4 для mxr_memset4 */
    _Static_assert(sizeof(mxr_status_t) % 4 == 0,
                   "mxr_status_t size must be multiple of 4 for mxr_memset4");

    /* ================================================================
     *  Alignment helper
     * ================================================================ */
    static inline size_t MXR_IRAM_INLINE_ATTR mxr_align4(size_t bytes)
    {
        return (bytes + MXR_ALIGN_MASK) & ~(size_t)MXR_ALIGN_MASK;
    }

    /* ================================================================
     *  Descriptor helpers
     * ================================================================ */
    static inline uint32_t MXR_IRAM_INLINE_ATTR mxr_desc_off(const mxr_desc_t *d)
    {
        return d->off_flags & MXR_OFF_MASK;
    }

    static inline uint32_t MXR_IRAM_INLINE_ATTR mxr_desc_len(const mxr_desc_t *d)
    {
        return d->len_flags & MXR_LEN_MASK;
    }

    static inline bool MXR_IRAM_INLINE_ATTR mxr_desc_is_exec(const mxr_desc_t *d)
    {
        return (d->len_flags & MXR_LEN_FLAG_EXEC) != 0;
    }

    static inline void MXR_IRAM_INLINE_ATTR mxr_desc_clear(mxr_desc_t *d)
    {
        d->off_flags = 0;
        d->len_flags = 0;
    }

    static inline void MXR_IRAM_INLINE_ATTR mxr_desc_set(
        mxr_desc_t *d,
        uint32_t off_bytes,
        uint32_t len_bytes,
        uint32_t len_flags)
    {
        d->off_flags = off_bytes & MXR_OFF_MASK;
        d->len_flags = (len_bytes & MXR_LEN_MASK) | (len_flags & MXR_LEN_FLAGS_MASK);
    }

    /* ================================================================
     *  MxR API
     * ================================================================ */
    void mxr_init(void);
    void *mxr_malloc(size_t size);
    void mxr_free(void *ptr);
    void *mxr_calloc(size_t count, size_t size);
    void *mxr_realloc(void *ptr, size_t size);
    void *mxr_zalloc(size_t size);
    void mxr_get_status(mxr_status_t *status);
    bool mxr_get_region_status(int region_index, mxr_region_status_t *status);
    bool mxr_get_iram_fb_region_status(int region_index, mxr_region_status_t *status);
    void mxr_dump(void);

    /* ESP heap compatibility layer */
    void _heap_caps_free(void *ptr, const char *file, size_t line);
    void *_heap_caps_malloc(size_t size, uint32_t caps, const char *file, size_t line);
    void *_heap_caps_calloc(size_t count, size_t size, uint32_t caps, const char *file, size_t line);
    void *_heap_caps_realloc(void *mem, size_t newsize, uint32_t caps, const char *file, size_t line);
    void *_heap_caps_zalloc(size_t size, uint32_t caps, const char *file, size_t line);
    size_t heap_caps_get_free_size(uint32_t caps);
    size_t heap_caps_get_minimum_free_size(uint32_t caps);
    size_t heap_caps_get_dram_free_size(void);
    void heap_caps_init(void);
    void *heap_caps_malloc_default(size_t size);
    void *heap_caps_realloc_default(void *ptr, size_t size);

    /* Capability-aware MxR API */
    void *mxr_malloc_caps(size_t size, uint32_t caps);
    void *mxr_calloc_caps(size_t count, size_t size, uint32_t caps);
    void *mxr_realloc_caps(void *ptr, size_t newsize, uint32_t caps);
    void *mxr_zalloc_caps(size_t size, uint32_t caps);
    size_t mxr_get_free_size_caps(uint32_t caps);
    size_t mxr_get_min_free_size_caps(uint32_t caps);

#ifdef __cplusplus
}
#endif