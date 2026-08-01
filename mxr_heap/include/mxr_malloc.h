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

    /*
     * realloc(ptr, 0) behavior:
     *
     * MXR_REALLOC_ZERO_FREES   = 1 : free(ptr), return NULL  (glibc / ESP-IDF style)
     * MXR_REALLOC_ZERO_FREES   = 0 : allocate minimal block  (current MxR behavior)
     */
#ifndef MXR_REALLOC_ZERO_FREES
#define MXR_REALLOC_ZERO_FREES 1
#endif

    /*
     * MxR-malloc for ESP8266 RTOS SDK.
     *
     * Descriptor format:
     *
     *   off_flags:
     *     bit 15     = MXR_OFF_FLAG_IRAM (0=DRAM, 1=IRAM)
     *     bits 14..0 = offset in 4-byte units
     *
     *   len_flags:
     *     bit 15     = MXR_LEN_FLAG_EXEC (1=EXEC block)
     *     bits 14..0 = length in 4-byte units minus 1
     *
     * Max arena size:
     *   32768 units * 4 = 131072 bytes
     */

#ifndef CONFIG_MXR_MAX_DESC
#define CONFIG_MXR_MAX_DESC 256
#endif

#ifndef CONFIG_MXR_REGIONS
#define CONFIG_MXR_REGIONS 3
#endif

#define MXR_USER_REGIONS CONFIG_MXR_REGIONS

#define MXR_UNIT_SIZE 4

#define MXR_OFF_BITS 15
#define MXR_LEN_BITS 15

#define MXR_OFF_MASK ((uint16_t)((1u << MXR_OFF_BITS) - 1u))
#define MXR_LEN_MASK ((uint16_t)((1u << MXR_LEN_BITS) - 1u))

#define MXR_OFF_FLAGS_MASK ((uint16_t)~MXR_OFF_MASK)
#define MXR_LEN_FLAGS_MASK ((uint16_t)~MXR_LEN_MASK)

    /*
     * Arena marker in off_flags.
     */
#define MXR_OFF_FLAG_IRAM ((uint16_t)(1u << 15))

    /*
     * Block type marker in len_flags.
     */
#define MXR_LEN_FLAG_EXEC ((uint16_t)(1u << 15))

#define MXR_MAX_OFFSET_UNITS MXR_OFF_MASK
#define MXR_MAX_LEN_UNITS ((uint16_t)(1u << MXR_LEN_BITS))
#define MXR_MAX_LEN_BYTES ((size_t)MXR_MAX_LEN_UNITS * MXR_UNIT_SIZE)

#define MXR_MAX_ARENA_UNITS MXR_MAX_LEN_UNITS
#define MXR_MAX_ARENA_BYTES ((size_t)MXR_MAX_ARENA_UNITS * MXR_UNIT_SIZE)

#define MXR_REGION_MAX_UNLIMITED 0

    /*
     * Compatibility capability bits.
     * Values match ESP8266 RTOS SDK / ESP-IDF.
     */
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

    /*
     * DRAM capabilities.
     */
#define MXR_DRAM_CAPS_DEFAULT \
    (MALLOC_CAP_8BIT | MALLOC_CAP_32BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)

    /*
     * IRAM capabilities.
     */
#define MXR_IRAM_CAPS_DEFAULT \
    (MALLOC_CAP_32BIT | MALLOC_CAP_EXEC)

    /*
     * Allocation descriptor. Always 4 bytes.
     */
    typedef struct
    {
        uint16_t off_flags;
        uint16_t len_flags;
    } mxr_desc_t;

    /*
     * Region configuration.
     */
    typedef struct
    {
        uint8_t percent;
        uint16_t min_units;
        uint16_t max_units;
    } mxr_region_cfg_t;

    /*
     * Runtime region state (DRAM only).
     */
    typedef struct
    {
        uint32_t caps;
        uint16_t start_unit;
        uint16_t total_units;
        uint16_t min_units;
        uint16_t max_units;
        uint16_t free_units;
        uint16_t min_free_units;
        uint16_t alloc_count;
    } mxr_region_t;

    /*
     * Region status for diagnostics.
     */
    typedef struct
    {
        uint32_t caps;
        uint16_t start_unit;
        uint16_t total_units;
        uint16_t min_units;
        uint16_t max_units;
        uint16_t free_units;
        uint16_t min_free_units;
        uint16_t largest_free_units;
        uint16_t alloc_count;
    } mxr_region_status_t;

    /*
     * Global allocator status.
     */
    typedef struct
    {
        bool initialized;
        uint8_t region_count;
        uint16_t desc_capacity;
        uint16_t active_allocs;
        uint16_t max_active_allocs;
        size_t total_bytes;
        size_t free_bytes;
        size_t min_free_bytes;
        size_t largest_free_block_bytes;
        size_t iram_total_bytes;
        size_t iram_free_bytes;
        size_t iram_min_free_bytes;
        uint32_t exec_allocs;
        uint32_t iram_fallback_allocs;
        uint32_t cross_region_allocs;
        uint32_t alloc_fail_no_memory;
        uint32_t alloc_fail_table_full;
        uint32_t invalid_free_attempts;
        uint32_t cross_region_skip_fragmented;
    } mxr_status_t;

    /*
     * Descriptor helpers.
     */
    static inline uint16_t MXR_IRAM_INLINE_ATTR mxr_desc_off(const mxr_desc_t *d)
    {
        return (uint16_t)(d->off_flags & MXR_OFF_MASK);
    }

    static inline uint16_t MXR_IRAM_INLINE_ATTR mxr_desc_len(const mxr_desc_t *d)
    {
        return (uint16_t)((d->len_flags & MXR_LEN_MASK) + 1u);
    }

    static inline uint16_t MXR_IRAM_INLINE_ATTR mxr_desc_off_flags(const mxr_desc_t *d)
    {
        return (uint16_t)(d->off_flags & MXR_OFF_FLAGS_MASK);
    }

    static inline uint16_t MXR_IRAM_INLINE_ATTR mxr_desc_len_flags(const mxr_desc_t *d)
    {
        return (uint16_t)(d->len_flags & MXR_LEN_FLAGS_MASK);
    }

    static inline bool MXR_IRAM_INLINE_ATTR mxr_desc_is_iram(const mxr_desc_t *d)
    {
        return (d->off_flags & MXR_OFF_FLAG_IRAM) != 0;
    }

    static inline bool MXR_IRAM_INLINE_ATTR mxr_desc_is_exec(const mxr_desc_t *d)
    {
        return (d->len_flags & MXR_LEN_FLAG_EXEC) != 0;
    }

    static inline uint16_t MXR_IRAM_INLINE_ATTR mxr_desc_make_key(uint16_t off_units, bool iram)
    {
        if (iram)
        {
            return (uint16_t)(off_units | MXR_OFF_FLAG_IRAM);
        }
        return off_units;
    }

    static inline void MXR_IRAM_INLINE_ATTR mxr_desc_clear(mxr_desc_t *d)
    {
        d->off_flags = 0;
        d->len_flags = 0;
    }

    static inline void MXR_IRAM_INLINE_ATTR mxr_desc_set(
        mxr_desc_t *d,
        uint16_t off_units,
        uint16_t len_units,
        uint16_t off_flags,
        uint16_t len_flags)
    {
        uint16_t stored_len;
        if (len_units == 0)
        {
            stored_len = 0;
        }
        else
        {
            stored_len = (uint16_t)(len_units - 1u);
        }
        d->off_flags = (uint16_t)((off_units & MXR_OFF_MASK) |
                                  (off_flags & MXR_OFF_FLAGS_MASK));
        d->len_flags = (uint16_t)((stored_len & MXR_LEN_MASK) |
                                  (len_flags & MXR_LEN_FLAGS_MASK));
    }

    /*
     * MxR API.
     */
    void mxr_init(void);
    void *mxr_malloc(size_t size);
    void mxr_free(void *ptr);
    void *mxr_calloc(size_t count, size_t size);
    void *mxr_realloc(void *ptr, size_t size);
    void *mxr_zalloc(size_t size);
    void mxr_get_status(mxr_status_t *status);
    bool mxr_get_region_status(int region_index, mxr_region_status_t *status);
    void mxr_dump(void);

    /*
     * ESP heap compatibility layer.
     */
    void *_heap_caps_malloc(size_t size, uint32_t caps, const char *file, size_t line);
    void _heap_caps_free(void *ptr, const char *file, size_t line);
    void *_heap_caps_calloc(size_t count, size_t size, uint32_t caps, const char *file, size_t line);
    void *_heap_caps_realloc(void *mem, size_t newsize, uint32_t caps, const char *file, size_t line);
    void *_heap_caps_zalloc(size_t size, uint32_t caps, const char *file, size_t line);
    size_t heap_caps_get_free_size(uint32_t caps);
    size_t heap_caps_get_minimum_free_size(uint32_t caps);
    size_t heap_caps_get_dram_free_size(void);
    void heap_caps_init(void);
    void *heap_caps_malloc_default(size_t size);
    void *heap_caps_realloc_default(void *ptr, size_t size);

    /*
     * Capability-aware MxR API.
     */
    void *mxr_malloc_caps(size_t size, uint32_t caps);
    void *mxr_calloc_caps(size_t count, size_t size, uint32_t caps);
    void *mxr_realloc_caps(void *ptr, size_t newsize, uint32_t caps);
    void *mxr_zalloc_caps(size_t size, uint32_t caps);
    size_t mxr_get_free_size_caps(uint32_t caps);
    size_t mxr_get_min_free_size_caps(uint32_t caps);

#ifdef __cplusplus
}
#endif