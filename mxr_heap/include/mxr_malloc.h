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
 *   1 = free(ptr), return NULL   (glibc / ESP-IDF)
 *   0 = allocate minimal block
 */
#ifndef MXR_REALLOC_ZERO_FREES
#define MXR_REALLOC_ZERO_FREES 1
#endif

/* ================================================================
 *  MxR-malloc v2 for ESP8266 RTOS SDK
 *
 *  Descriptor format (8 bytes, stored in IRAM):
 *
 *    off_flags  (uint32_t):
 *      bit 31      = MXR_OFF_FLAG_IRAM  (0 = DRAM, 1 = IRAM)
 *      bits 30..0  = offset in bytes from arena base
 *
 *    len_flags  (uint32_t):
 *      bit 31      = MXR_LEN_FLAG_EXEC  (1 = EXEC block)
 *      bits 30..0  = length in bytes
 *
 *  All offsets and lengths are in bytes, 4-byte aligned.
 * ================================================================ */

#ifndef CONFIG_MXR_MAX_DESC
#define CONFIG_MXR_MAX_DESC 256
#endif

#define MXR_REGIONS_MAX  16
#define MXR_REGIONS_MIN  1

#ifndef CONFIG_MXR_REGIONS
#define CONFIG_MXR_REGIONS 3
#endif

#if CONFIG_MXR_REGIONS > MXR_REGIONS_MAX
#undef CONFIG_MXR_REGIONS
#define CONFIG_MXR_REGIONS MXR_REGIONS_MAX
#elif CONFIG_MXR_REGIONS < MXR_REGIONS_MIN
#undef CONFIG_MXR_REGIONS
#define CONFIG_MXR_REGIONS MXR_REGIONS_MIN
#endif




#define MXR_USER_REGIONS CONFIG_MXR_REGIONS

/* Alignment */
#define MXR_ALIGN_SIZE  4
#define MXR_ALIGN_MASK  (MXR_ALIGN_SIZE - 1)

/* Descriptor bit layout (32-bit fields) */
#define MXR_OFF_BITS    31
#define MXR_LEN_BITS    31

#define MXR_OFF_MASK    ((uint32_t)((1u << MXR_OFF_BITS) - 1u))
#define MXR_LEN_MASK    ((uint32_t)((1u << MXR_LEN_BITS) - 1u))

#define MXR_OFF_FLAGS_MASK  ((uint32_t)~MXR_OFF_MASK)
#define MXR_LEN_FLAGS_MASK  ((uint32_t)~MXR_LEN_MASK)

#define MXR_OFF_FLAG_IRAM   ((uint32_t)(1u << 31))
#define MXR_LEN_FLAG_EXEC   ((uint32_t)(1u << 31))

#define MXR_MAX_OFFSET_BYTES  MXR_OFF_MASK
#define MXR_MAX_LEN_BYTES     ((size_t)MXR_LEN_MASK)
#define MXR_MAX_ARENA_BYTES   MXR_MAX_LEN_BYTES

#define MXR_REGION_MAX_UNLIMITED  0

/* ================================================================
 *  Platform-dependent type widths
 *
 *  ESP8266 compact:
 *    Arena <= 128 KB   -> offset / size need uint32 (17 bits)
 *    Size class <= 64 KB -> min / max need uint16
 *    Alloc count <= 4096 -> uint16
 *    Caps bits 0..11    -> uint16
 *
 *  ESP32 / generous:
 *    uint32 everywhere.
 * ================================================================ */
#ifdef CONFIG_MXR_COMPACT_TYPES

typedef uint16_t mxr_caps_t;
typedef uint16_t mxr_class_t;    /* min_bytes, max_bytes (0 = unlimited) */
typedef uint16_t mxr_count_t;    /* alloc_count */
typedef uint32_t mxr_offset_t;   /* start_byte, descriptor offset */
typedef uint32_t mxr_size_t;     /* total, free, min_free, largest */

#else /* ESP32 / generous */

typedef uint32_t mxr_caps_t;
typedef uint32_t mxr_class_t;
typedef uint32_t mxr_count_t;
typedef uint32_t mxr_offset_t;
typedef uint32_t mxr_size_t;

#endif

/* ================================================================
 *  Capability bits (match ESP8266 RTOS SDK / ESP-IDF)
 * ================================================================ */
#ifndef MALLOC_CAP_EXEC
#define MALLOC_CAP_EXEC      (1 << 0)
#endif
#ifndef MALLOC_CAP_32BIT
#define MALLOC_CAP_32BIT     (1 << 1)
#endif
#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT      (1 << 2)
#endif
#ifndef MALLOC_CAP_DMA
#define MALLOC_CAP_DMA       (1 << 3)
#endif
#ifndef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM    (1 << 10)
#endif
#ifndef MALLOC_CAP_INTERNAL
#define MALLOC_CAP_INTERNAL  (1 << 11)
#endif

#define MXR_DRAM_CAPS_DEFAULT \
    (MALLOC_CAP_8BIT | MALLOC_CAP_32BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)

#define MXR_IRAM_CAPS_DEFAULT \
    (MALLOC_CAP_32BIT | MALLOC_CAP_EXEC)


    /*
 * Data placement attribute.
 * Follows the same placement as s_desc[].
 */
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
    uint32_t off_flags;   /* bit31 = IRAM, bits30..0 = offset bytes */
    uint32_t len_flags;   /* bit31 = EXEC, bits30..0 = length bytes */
} mxr_desc_t;
_Static_assert(sizeof(mxr_desc_t) == 8, "desc must be 8 bytes");
/* ================================================================
 *  Region configuration (build-time)
 * ================================================================ */
typedef struct
{
    uint8_t     percent;
    mxr_class_t min_bytes;
    mxr_class_t max_bytes;   /* 0 = MXR_REGION_MAX_UNLIMITED */
} mxr_region_cfg_t;

/* ================================================================
 *  Runtime region state (DRAM only)
 * ================================================================ */
typedef struct
{
    mxr_caps_t   caps;
    mxr_offset_t start_byte;
    mxr_size_t   total_bytes;
    mxr_class_t  min_bytes;
    mxr_class_t  max_bytes;      /* 0 = MXR_REGION_MAX_UNLIMITED */
    mxr_size_t   free_bytes;
    mxr_size_t   min_free_bytes;
    mxr_count_t  alloc_count;
} mxr_region_t;

/* ================================================================
 *  Region status for diagnostics
 * ================================================================ */
typedef struct
{
    mxr_caps_t   caps;
    mxr_offset_t start_byte;
    mxr_size_t   total_bytes;
    mxr_class_t  min_bytes;
    mxr_class_t  max_bytes;
    mxr_size_t   free_bytes;
    mxr_size_t   min_free_bytes;
    mxr_size_t   largest_free_bytes;
    mxr_count_t  alloc_count;
} mxr_region_status_t;

/* ================================================================
 *  Global allocator status
 * ================================================================ */
typedef struct
{
    bool     initialized;
    uint8_t  region_count;
    uint16_t desc_capacity;
    uint16_t active_allocs;
    uint16_t max_active_allocs;

    size_t   total_bytes;
    size_t   free_bytes;
    size_t   min_free_bytes;
    size_t   largest_free_block_bytes;

    size_t   iram_total_bytes;
    size_t   iram_free_bytes;
    size_t   iram_min_free_bytes;

    uint32_t exec_allocs;
    uint32_t iram_fallback_allocs;
    uint32_t cross_region_allocs;
    uint32_t alloc_fail_no_memory;
    uint32_t alloc_fail_table_full;
    uint32_t invalid_free_attempts;
    uint32_t cross_region_skip_fragmented;
} mxr_status_t;

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

static inline uint32_t MXR_IRAM_INLINE_ATTR mxr_desc_off_flags(const mxr_desc_t *d)
{
    return d->off_flags & MXR_OFF_FLAGS_MASK;
}

static inline uint32_t MXR_IRAM_INLINE_ATTR mxr_desc_len_flags(const mxr_desc_t *d)
{
    return d->len_flags & MXR_LEN_FLAGS_MASK;
}

static inline bool MXR_IRAM_INLINE_ATTR mxr_desc_is_iram(const mxr_desc_t *d)
{
    return (d->off_flags & MXR_OFF_FLAG_IRAM) != 0;
}

static inline bool MXR_IRAM_INLINE_ATTR mxr_desc_is_exec(const mxr_desc_t *d)
{
    return (d->len_flags & MXR_LEN_FLAG_EXEC) != 0;
}

static inline uint32_t MXR_IRAM_INLINE_ATTR mxr_desc_make_key(uint32_t off_bytes, bool iram)
{
    if (iram)
    {
        return off_bytes | MXR_OFF_FLAG_IRAM;
    }
    return off_bytes;
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
    uint32_t off_flags,
    uint32_t len_flags)
{
    d->off_flags = (off_bytes & MXR_OFF_MASK) | (off_flags & MXR_OFF_FLAGS_MASK);
    d->len_flags = (len_bytes & MXR_LEN_MASK) | (len_flags & MXR_LEN_FLAGS_MASK);
}

/* ================================================================
 *  MxR API
 * ================================================================ */
void  mxr_init(void);
void *mxr_malloc(size_t size);
void  mxr_free(void *ptr);
void *mxr_calloc(size_t count, size_t size);
void *mxr_realloc(void *ptr, size_t size);
void *mxr_zalloc(size_t size);

void  mxr_get_status(mxr_status_t *status);
bool  mxr_get_region_status(int region_index, mxr_region_status_t *status);
void  mxr_dump(void);

/* ESP heap compatibility layer */
void  _heap_caps_free(void *ptr, const char *file, size_t line);
void *_heap_caps_malloc(size_t size, uint32_t caps, const char *file, size_t line);
void *_heap_caps_calloc(size_t count, size_t size, uint32_t caps, const char *file, size_t line);
void *_heap_caps_realloc(void *mem, size_t newsize, uint32_t caps, const char *file, size_t line);
void *_heap_caps_zalloc(size_t size, uint32_t caps, const char *file, size_t line);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_dram_free_size(void);
void   heap_caps_init(void);
void  *heap_caps_malloc_default(size_t size);
void  *heap_caps_realloc_default(void *ptr, size_t size);

/* Capability-aware MxR API */
void  *mxr_malloc_caps(size_t size, uint32_t caps);
void  *mxr_calloc_caps(size_t count, size_t size, uint32_t caps);
void  *mxr_realloc_caps(void *ptr, size_t newsize, uint32_t caps);
void  *mxr_zalloc_caps(size_t size, uint32_t caps);
size_t mxr_get_free_size_caps(uint32_t caps);
size_t mxr_get_min_free_size_caps(uint32_t caps);


#ifdef __cplusplus
}
#endif