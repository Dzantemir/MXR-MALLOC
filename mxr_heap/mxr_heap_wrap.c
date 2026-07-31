#include "mxr_malloc.h"

#include <stdint.h>
#include <stddef.h>

#include "esp_attr.h"

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

/*
 * If CONFIG_MXR_IRAM_HOT_PATH_DISABLED is set, wrapper functions
 * are placed in flash.
 *
 * Otherwise critical malloc/free wrappers are placed in IRAM.
 */
#ifdef CONFIG_MXR_IRAM_HOT_PATH_DISABLED
#define MXR_WRAP_IRAM
#else
#define MXR_WRAP_IRAM IRAM_ATTR
#endif

/*
 * MxR-malloc linker wrap layer.
 *
 * Use this file INSTEAD of mxr_heap_compat.c and mxr_heap_port.c
 * when the original heap component remains in the build.
 *
 * How it works:
 *
 *   Application calls:       _heap_caps_malloc(...)
 *   Linker redirects to:     __wrap__heap_caps_malloc(...)
 *   Which calls:             mxr_malloc_caps(...)
 *
 * IMPORTANT:
 *   When using this wrap module, do NOT compile:
 *
 *       mxr_heap_compat.c
 *       mxr_heap_port.c
 *
 *   They would conflict with the original heap component.
 */

/* ================================================================
 *  Base wraps
 *
 *  These are always enabled in CMakeLists.txt:
 *
 *    heap_caps_init
 *    _heap_caps_malloc
 *    _heap_caps_free
 *    _heap_caps_realloc
 *    _heap_caps_calloc
 *    _heap_caps_zalloc
 * ================================================================ */

void __wrap_heap_caps_init(void)
{
    mxr_init();
}

void *MXR_WRAP_IRAM __wrap__heap_caps_malloc(
    size_t size,
    uint32_t caps,
    const char *file,
    size_t line)
{
    (void)file;
    (void)line;

    return mxr_malloc_caps(size, caps);
}

void MXR_WRAP_IRAM __wrap__heap_caps_free(
    void *ptr,
    const char *file,
    size_t line)
{
    (void)file;
    (void)line;

    mxr_free(ptr);
}

void *__wrap__heap_caps_realloc(
    void *mem,
    size_t newsize,
    uint32_t caps,
    const char *file,
    size_t line)
{
    (void)file;
    (void)line;

    return mxr_realloc_caps(mem, newsize, caps);
}

void *__wrap__heap_caps_calloc(
    size_t count,
    size_t size,
    uint32_t caps,
    const char *file,
    size_t line)
{
    (void)file;
    (void)line;

    return mxr_calloc_caps(count, size, caps);
}

void *__wrap__heap_caps_zalloc(
    size_t size,
    uint32_t caps,
    const char *file,
    size_t line)
{
    (void)file;
    (void)line;

    return mxr_zalloc_caps(size, caps);
}

/* ================================================================
 *  Heap query wraps
 *
 *  Enabled by:
 *
 *    CONFIG_MXR_WRAP_HEAP_QUERY
 *
 *  CMake wraps:
 *
 *    heap_caps_get_free_size
 *    heap_caps_get_minimum_free_size
 *    heap_caps_get_dram_free_size
 * ================================================================ */

#ifdef CONFIG_MXR_WRAP_HEAP_QUERY

size_t __wrap_heap_caps_get_free_size(uint32_t caps)
{
    return mxr_get_free_size_caps(caps);
}

size_t __wrap_heap_caps_get_minimum_free_size(uint32_t caps)
{
    return mxr_get_min_free_size_caps(caps);
}

size_t MXR_WRAP_IRAM __wrap_heap_caps_get_dram_free_size(void)
{
    return mxr_get_free_size_caps(
        MALLOC_CAP_8BIT | MALLOC_CAP_32BIT | MALLOC_CAP_DMA);
}

#endif /* CONFIG_MXR_WRAP_HEAP_QUERY */

/* ================================================================
 *  Default pool wraps
 *
 *  Enabled by:
 *
 *    CONFIG_MXR_WRAP_DEFAULT_POOL
 *
 *  CMake wraps:
 *
 *    heap_caps_malloc_default
 *    heap_caps_realloc_default
 * ================================================================ */

#ifdef CONFIG_MXR_WRAP_DEFAULT_POOL

void *__wrap_heap_caps_malloc_default(size_t size)
{
    return mxr_malloc_caps(size, MALLOC_CAP_32BIT);
}

void *__wrap_heap_caps_realloc_default(void *ptr, size_t size)
{
    return mxr_realloc_caps(ptr, size, MALLOC_CAP_32BIT);
}

#endif /* CONFIG_MXR_WRAP_DEFAULT_POOL */

/* ================================================================
 *  ESP system heap wraps
 *
 *  Enabled by:
 *
 *    CONFIG_MXR_WRAP_ESP_SYSTEM
 *
 *  CMake wraps:
 *
 *    esp_get_free_heap_size
 *    esp_get_minimum_free_heap_size
 *    esp_get_free_internal_heap_size
 * ================================================================ */

#ifdef CONFIG_MXR_WRAP_ESP_SYSTEM

size_t __wrap_esp_get_free_heap_size(void)
{
    return mxr_get_free_size_caps(MALLOC_CAP_32BIT);
}

size_t __wrap_esp_get_minimum_free_heap_size(void)
{
    return mxr_get_min_free_size_caps(MALLOC_CAP_32BIT);
}

size_t __wrap_esp_get_free_internal_heap_size(void)
{
    return mxr_get_free_size_caps(MALLOC_CAP_INTERNAL);
}

#endif /* CONFIG_MXR_WRAP_ESP_SYSTEM */

/* ================================================================
 *  Optional libc wraps
 *
 *  Enabled by:
 *
 *    CONFIG_MXR_WRAP_LIBC
 *
 *  CMake wraps:
 *
 *    malloc
 *    free
 *    calloc
 *    realloc
 *    zalloc
 *
 *  WARNING:
 *    Enable this only if you understand your SDK/newlib path.
 *    It may conflict with original heap tracing or custom libc hooks.
 * ================================================================ */

#ifdef CONFIG_MXR_WRAP_LIBC

void *MXR_WRAP_IRAM __wrap_malloc(size_t n)
{
    return mxr_malloc_caps(n, MALLOC_CAP_32BIT);
}

void MXR_WRAP_IRAM __wrap_free(void *ptr)
{
    mxr_free(ptr);
}

void *__wrap_calloc(size_t c, size_t s)
{
    return mxr_calloc_caps(c, s, MALLOC_CAP_32BIT);
}

void *__wrap_realloc(void *old_ptr, size_t n)
{
    return mxr_realloc_caps(old_ptr, n, MALLOC_CAP_32BIT);
}

void *__wrap_zalloc(size_t n)
{
    return mxr_zalloc_caps(n, MALLOC_CAP_32BIT);
}

#endif /* CONFIG_MXR_WRAP_LIBC */