# MxR-malloc

<p align="center">
  <img src="https://img.shields.io/badge/ESP8266-RTOS%20SDK%20v3.4-blue" alt="ESP8266 RTOS SDK">
  <img src="https://img.shields.io/badge/language-C99-green" alt="C99">
  <img src="https://img.shields.io/badge/license-MIT-yellow" alt="License">
  <img src="https://img.shields.io/badge/platform-Xtensa%20LX106-orange" alt="Platform">
</p>

<p align="center">
  <b>Compact, descriptor-based memory allocator for ESP8266 RTOS SDK</b><br>
  with size-class regions, IRAM fallback, cross-region fallback, and optional bitmap acceleration.
</p>

---

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Architecture](#architecture)
- [Memory Layout](#memory-layout)
- [Allocation Policy](#allocation-policy)
- [Descriptor Format](#descriptor-format)
- [Region Configuration](#region-configuration)
- [Search Modes](#search-modes)
- [IRAM Support](#iram-support)
- [Cross-Region Fallback](#cross-region-fallback)
- [Installation](#installation)
- [Configuration](#configuration)
- [API Reference](#api-reference)
- [Usage Examples](#usage-examples)
- [Comparison with Original Heap](#comparison-with-original-heap)
- [Performance Considerations](#performance-considerations)
- [Diagnostics](#diagnostics)
- [Project Structure](#project-structure)
- [FAQ](#faq)
- [License](#license)

---

## Overview

**MxR-malloc** is a drop-in replacement for the default ESP8266 RTOS SDK heap
allocator. Instead of in-band block headers it keeps all metadata in an
**out-of-band descriptor table**, giving exact 4-byte alignment with zero
per-block overhead inside the arena.

The allocator manages two physical arenas:

| Arena | Address Range | Capabilities |
|-------|--------------|--------------|
| **DRAM** | `_bss_end` -> `0x40000000` | `8BIT` / `32BIT` / `DMA` / `INTERNAL` |
| **IRAM** | `_iram_end` -> `0x40100000 + SOC_IRAM_SIZE` | `32BIT` / `EXEC` |

Ordinary allocations go to **DRAM first**. When DRAM is exhausted, 32-bit
allocations **fall back to IRAM**. Executable memory (`MALLOC_CAP_EXEC`) is
served **exclusively from IRAM**.

---

## Features

- **Zero in-band overhead** — metadata lives in a separate descriptor table.
- **Exact 4-byte alignment** — every allocation is naturally aligned.
- **Size-class regions** — DRAM is split into configurable regions by block size.
- **IRAM fallback** — unused IRAM is reclaimed as a 32-bit fallback pool.
- **Cross-region DRAM fallback (opt-in)** — when a size-class region and IRAM
  are both exhausted, allocation can spill into other DRAM regions as a last
  resort (disabled by default to avoid fragmentation).
- **EXEC support** — `MALLOC_CAP_EXEC` allocations come from IRAM only.
- **Two search modes** — descriptor gap search (default) or bitmap search
  (carved from the arena, no fixed 4 KB cost).
- **Linker `--wrap` integration** — replaces the heap without touching the SDK.
- **Full Kconfig integration** — every parameter is set via `menuconfig`.
- **Rich diagnostics** — per-region stats, fallback counters, heap dump.
- **IRAM-safe hot path** — `malloc`/`free` can be placed in IRAM.
- **Word-wise copy/clear** — `realloc`/`calloc`/`zalloc` never emit byte stores
  into IRAM (IRAM is 32-bit access only).

---

## Architecture

```text
+-------------------------------------------------------------+
|                    Application / SDK                        |
|         malloc / calloc / realloc / heap_caps_malloc        |
+---------------------------+---------------------------------+
                            |  linker --wrap
                            v
+-------------------------------------------------------------+
|                    MxR-malloc core                          |
|                                                             |
|  +--------------+  +--------------+  +------------------+   |
|  |  DRAM arena  |  |  IRAM arena  |  | Descriptor table |   |
|  |  (regions)   |  |  (flat)      |  | (sorted, 4B/ent) |   |
|  +--------------+  +--------------+  +------------------+   |
|                                                             |
|  +--------------+  +--------------+  +------------------+   |
|  | Bitmap (opt) |  |  Statistics  |  |  Kconfig parser  |   |
|  +--------------+  +--------------+  +------------------+   |
+-------------------------------------------------------------+
```

A single sorted descriptor table tracks blocks from **both** arenas. The arena
is encoded in the top bit of the offset field, so all DRAM descriptors sort
before all IRAM descriptors and one binary search serves both.

### Allocation flow

```text
heap_caps_malloc(size, caps)
        |
        v
  +-----------+
  |   caps?   |
  +-----+-----+
        |
  +-----+------------------+-------------------+
  v                        v                   v
EXEC only             DMA / 8BIT          32BIT / default
(IRAM, start)         (DRAM only)         (DRAM size-class)
  |                        |                   |
  v                        v                   v
IRAM success?         DRAM success?       DRAM success?
  |                        |                   |
  +-- yes -> IRAM ptr      +-- yes -> DRAM ptr |
  |                        |                   |
  +-- no -> NULL           +-- no -> NULL      v
                                          IRAM fallback?
                                               |
                                          +----+----+
                                          |         |
                                         yes       no
                                          |         |
                                          v         v
                                    IRAM success?  cross-region
                                          |        fallback?
                                     +----+----+       |
                                     |         |  +----+----+
                                    yes       no  |         |
                                     |         | yes       no
                                     v         v  |         |
                               IRAM ptr       cross-region  v
                                              success?    NULL
                                                  |
                                             +----+----+
                                             |         |
                                            yes       no
                                             |         |
                                             v         v
                                        DRAM ptr     NULL
```

---

## Memory Layout

### DRAM arena

```text
0x3FFE8000                                        0x40000000
    |  .data .bss |        DRAM heap arena             |
    |<----------->|<---------------------------------->|
    |  (firmware) |  +--------+--------+-----------+   |
    |             |  |Region 0|Region 1| Region 2  |   |
    |             |  | 4-128B |132-1020| 1024-max  |   |
    |             |  | (15%)  | (45%)  | (20%+rest)|   |
    |             |  +--------+--------+-----------+   |
    |             |  [ bitmap (if enabled, carved) ]   |
```

### IRAM arena

```text
0x40100000                              0x40100000 + SOC_IRAM_SIZE
    |  .iram.text |      IRAM heap arena      |
    |<----------->|<------------------------->|
    |   (code)    |  EXEC blocks grow -->     |
    |             |  <-- fallback blocks grow |
    |             |  [ reserve for EXEC ]     |
```

---

## Allocation Policy

| Request | Destination | Notes |
|---------|-------------|-------|
| `malloc(size)` | DRAM -> IRAM fallback | equivalent to `MALLOC_CAP_32BIT` |
| `MALLOC_CAP_32BIT` | DRAM -> IRAM fallback | ordinary 32-bit memory |
| `MALLOC_CAP_8BIT` | DRAM only | byte-accessible memory |
| `MALLOC_CAP_DMA` | DRAM only | DMA-capable memory |
| `MALLOC_CAP_INTERNAL` | DRAM only | internal RAM |
| `MALLOC_CAP_EXEC` | IRAM only | executable memory |
| `MALLOC_CAP_EXEC \| 32BIT` | IRAM only | executable 32-bit memory |
| `MALLOC_CAP_SPIRAM` | `NULL` | not supported on ESP8266 |

### Fallback chain (32BIT / default)

```text
1. Own DRAM size-class region
2. IRAM fallback (if CONFIG_MXR_USE_IRAM=y)
3. Cross-region DRAM fallback (if CONFIG_MXR_CROSS_REGION_FALLBACK=y)
4. NULL
```

### IRAM fallback rules

```text
IRAM fallback is allowed when ALL of:
  - CONFIG_MXR_USE_IRAM = y
  - request does NOT include EXEC, DMA, 8BIT or SPIRAM
  - request includes 32BIT (or caps == 0)
  - IRAM free >= requested + CONFIG_MXR_IRAM_RESERVE_BYTES
  - requested size <= CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES (0 = unlimited)
```

---

## Descriptor Format

Each live block owns one 4-byte descriptor:

```text
  off_flags (16 bits)              len_flags (16 bits)
+---+-------------------+       +---+-------------------+
| A |   offset (15 bit) |       | E |  length-1 (15 bit)|
+---+-------------------+       +---+-------------------+
  |                               |
  | A = 0 -> DRAM                 | E = 0 -> normal / fallback
  | A = 1 -> IRAM                 | E = 1 -> EXEC block
  |                               |
  +- offset in 4-byte units       +- length in 4-byte units
     from arena base                 (stored as length - 1)
```

Block classes:

```text
DRAM block:          off bit15 = 0, len bit15 = 0
IRAM fallback block: off bit15 = 1, len bit15 = 0
IRAM EXEC block:     off bit15 = 1, len bit15 = 1
```

**Limits**

| Parameter | Value |
|-----------|-------|
| Max offset | 32 767 units (131 068 bytes) |
| Max block length | 32 768 units (131 072 bytes) |
| Max arena size | 32 768 units (131 072 bytes) |
| Alignment | 4 bytes |

---

## Region Configuration

DRAM regions are defined by their **lower boundaries only**, so gaps and
overlaps are impossible by construction. The last region is always unlimited.

```text
CONFIG_MXR_REGIONS=3
CONFIG_MXR_REGION_SIZES="4,132,1024"
CONFIG_MXR_REGION_PERCENTS="15,45,20"
```

creates:

| Region | Accepts | Memory |
|--------|---------|--------|
| 0 | 4 .. 128 B | 15% |
| 1 | 132 .. 1020 B | 45% |
| 2 | 1024 B .. max | 20% + remainder |

If the percent sum is below 100, the remainder is added to the last region.
If the last percent is `0`, the last region receives all remaining memory.

---

## Search Modes

### Descriptor gap search (default)

```text
CONFIG_MXR_SEARCH_DESCRIPTOR=y
```

Walks the sorted descriptor table and finds the first gap >= requested size.

- **Cost:** O(N) where N = active descriptors in the region
- **RAM:** 0 extra
- **Best for:** <= 512 active allocations

### Bitmap search (optional)

```text
CONFIG_MXR_SEARCH_BITMAP=y
```

Maintains a 1-bit-per-unit bitmap **carved from the end of the DRAM arena** at
init time, so its size scales with the actual arena instead of a fixed 4 KB.

```text
Arena:  [  allocatable units  ][ bitmap (carved) ]
         <--- s_arena_total --><--- s_bitmap --->

For 80 KB DRAM:
  80 000 / 4 = 20 000 units
  20 000 / 8 = 2 500 bytes bitmap
```

- **Cost:** O(arena_units / 32) word scans
- **RAM:** ~2.5 KB for an 80 KB arena (carved, not static)
- **Covers:** DRAM only — IRAM always uses descriptor search

---

## IRAM Support

```text
CONFIG_MXR_USE_IRAM=y                    # default y if !CONFIG_HEAP_DISABLE_IRAM
CONFIG_MXR_IRAM_RESERVE_BYTES=2048       # reserve for EXEC allocations
CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES=0     # 0 = unlimited fallback block size
```

- **EXEC allocations** grow from the **start** of IRAM.
- **Fallback allocations** grow from the **end** of IRAM.
- A **reserve** (default 2048 bytes) protects EXEC space from fallback.
- EXEC allocations ignore the reserve.
- IRAM is never used for `DMA` or `8BIT` requests.

Because IRAM on ESP8266 is 32-bit access only, the allocator uses word-wise
copy/clear (`mxr_memcpy_words` / `mxr_memset_words`) for any operation that may
touch IRAM, so `realloc`/`calloc`/`zalloc` never emit byte stores there.

---

## Cross-Region Fallback

**Disabled by default.** Enable only when you understand the fragmentation
trade-off.

```text
CONFIG_MXR_CROSS_REGION_FALLBACK=y
CONFIG_MXR_CROSS_REGION_AFTER_IRAM=y     # default y
```

### Why opt-in

Cross-region fallback lets a small block land in a large-block region (or vice
versa). This **fragments** the large-block region: once a 16-byte block sits in
the 1024+ region, that region can no longer serve a contiguous 16 KB buffer
even if it has 20 KB free.

Therefore cross-region is a **last resort**, tried only after:

1. The block's own size-class region failed.
2. IRAM fallback failed (or is disabled).

### Region selection

When cross-region fires, the allocator picks the DRAM region with the
**smallest `min_units`** that still has enough free space. This minimizes
damage: a 10-unit block prefers region 1 (min=33) over region 2 (min=256),
keeping region 2 intact for large buffers.

### Monitoring

Watch `cross_region_allocs` in `mxr_get_status()` / `mxr_dump()`:

```text
cross_region_allocs=17   <- if this grows, increase own region percent
```

If the counter grows steadily, your `MXR_REGION_PERCENTS` are misconfigured —
the own region is chronically undersized.

---

## Installation

### 1. Copy the component

```bash
cp -r mxr_heap/ <your_project>/components/mxr_heap/
```

### 2. Component structure

```text
components/mxr_heap/
|-- CMakeLists.txt
|-- Kconfig.projbuild
|-- include/
|   `-- mxr_malloc.h
|-- mxr_malloc.c
`-- mxr_heap_wrap.c
```

### 3. Configure

```bash
idf.py menuconfig
# -> MxR-malloc
```

### 4. Build

```bash
idf.py fullclean
idf.py build
```

> Always run `fullclean` after changing MxR Kconfig options. Stale `sdkconfig`
> values can otherwise override the new defaults.

---

## Configuration

### Global

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `MXR_MAX_DESC` | int (16-4096) | `256` | Max simultaneous allocations (shared DRAM+IRAM) |
| `MXR_IRAM_HOT_PATH_DISABLED` | bool | `n` | Place malloc/free hot path in flash instead of IRAM |

### IRAM

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `MXR_USE_IRAM` | bool | `y`* | Enable IRAM heap (EXEC + 32BIT fallback) |
| `MXR_IRAM_RESERVE_BYTES` | int (0-32768) | `2048` | IRAM reserved for EXEC allocations |
| `MXR_IRAM_FALLBACK_MAX_BYTES` | int (0-65536) | `0` | Max non-EXEC block allowed into IRAM (0=inf) |

\* default `y` if `CONFIG_HEAP_DISABLE_IRAM` is not set.

### Regions

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `MXR_REGIONS` | int (2-16) | `3` | Total number of DRAM regions |
| `MXR_REGION_SIZES` | string | `"4,132,1024"` | Lower block-size boundaries (bytes) |
| `MXR_REGION_PERCENTS` | string | `"15,45,20"` | Memory weight per region (%) |

### Cross-region fallback

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `MXR_CROSS_REGION_FALLBACK` | bool | `n` | Enable cross-region DRAM fallback (last resort) |
| `MXR_CROSS_REGION_AFTER_IRAM` | bool | `y` | Cross-region only after IRAM is exhausted |

### Search mode

| Option | Description |
|--------|-------------|
| `MXR_SEARCH_DESCRIPTOR` | Descriptor gap search (default, 0 extra RAM) |
| `MXR_SEARCH_BITMAP` | Bitmap search (carved from arena, DRAM only) |

### Linker integration

| Option | Default | Wraps |
|--------|---------|-------|
| `MXR_WRAP_HEAP_QUERY` | `y` | `heap_caps_get_free_size`, `..._minimum_free_size`, `..._dram_free_size` |
| `MXR_WRAP_DEFAULT_POOL` | `y` | `heap_caps_malloc_default`, `heap_caps_realloc_default` |
| `MXR_WRAP_ESP_SYSTEM` | `y` | `esp_get_free_heap_size`, `..._minimum`, `..._internal` |
| `MXR_WRAP_LIBC` | `n` | `malloc`, `free`, `calloc`, `realloc`, `zalloc` |
| `MXR_WARN_HEAP_TRACING` | `y` | Warn if `CONFIG_HEAP_TRACING` is enabled |

---

## API Reference

### Core API

```c
/* lifecycle + plain API */
void  mxr_init(void);
void *mxr_malloc(size_t size);
void  mxr_free(void *ptr);
void *mxr_calloc(size_t count, size_t size);
void *mxr_realloc(void *ptr, size_t size);
void *mxr_zalloc(size_t size);

/* capability-aware API */
void *mxr_malloc_caps(size_t size, uint32_t caps);
void *mxr_calloc_caps(size_t count, size_t size, uint32_t caps);
void *mxr_realloc_caps(void *ptr, size_t newsize, uint32_t caps);
void *mxr_zalloc_caps(size_t size, uint32_t caps);

/* query + diagnostics */
size_t mxr_get_free_size_caps(uint32_t caps);
size_t mxr_get_min_free_size_caps(uint32_t caps);
void   mxr_get_status(mxr_status_t *status);
bool   mxr_get_region_status(int region_index, mxr_region_status_t *status);
void   mxr_dump(void);
```

### ESP heap compatibility (via `--wrap`)

```text
heap_caps_malloc(size, caps)        -> mxr_malloc_caps(size, caps)
heap_caps_free(ptr)                 -> mxr_free(ptr)
heap_caps_calloc(n, size, caps)     -> mxr_calloc_caps(n, size, caps)
heap_caps_realloc(ptr, size, caps)  -> mxr_realloc_caps(ptr, size, caps)
heap_caps_zalloc(size, caps)        -> mxr_zalloc_caps(size, caps)
esp_get_free_heap_size()            -> mxr_get_free_size_caps(MALLOC_CAP_32BIT)
heap_caps_get_dram_free_size()      -> mxr_get_free_size_caps(8BIT|32BIT|DMA)
```

---

## Usage Examples

### Basic usage

```c
#include "mxr_malloc.h"

void app_main(void)
{
    /* ordinary allocation: DRAM first, IRAM fallback if needed */
    char *buf = malloc(1024);
    if (buf) {
        memset(buf, 0, 1024);
        free(buf);
    }

    /* DMA-safe buffer: always DRAM */
    void *dma = heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_32BIT);
    if (dma) heap_caps_free(dma);

    /* executable memory: IRAM only */
    void *code = heap_caps_malloc(256, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    if (code) heap_caps_free(code);
}
```

### Diagnostics

```c
mxr_status_t st;
mxr_get_status(&st);
printf("free=%u min=%u desc=%u/%u exec=%u fallback=%u cross=%u invalid_free=%u\n",
       (unsigned)st.free_bytes,
       (unsigned)st.min_free_bytes,
       (unsigned)st.active_allocs,
       (unsigned)st.desc_capacity,
       (unsigned)st.exec_allocs,
       (unsigned)st.iram_fallback_allocs,
       (unsigned)st.cross_region_allocs,
       (unsigned)st.invalid_free_attempts);
```

### Full heap dump

```c
/* Prints all regions and descriptors to UART */
mxr_dump();
```

---

## Comparison with Original Heap

| Feature | Original ESP8266 heap | MxR-malloc |
|---------|----------------------|------------|
| Metadata location | in-band (8 B/block) | out-of-band table (4 B/entry) |
| Free-space structure | linked free list per region | sorted descriptors + optional bitmap |
| Allocation search | O(N) list walk | O(N) gap scan or O(W) bitmap words |
| Max allocations | memory-bound | `CONFIG_MXR_MAX_DESC` (hard cap) |
| Size-class regions | no | yes (configurable) |
| IRAM usage | IRAM tried first for 32BIT | DRAM first, IRAM as fallback |
| Cross-region fallback | no | yes (opt-in, last resort) |
| Heap tracing | `CONFIG_HEAP_TRACING` | not supported (disable it) |
| Per-block heap overhead | 8 bytes | 0 bytes in arena |

### Key behavioral differences

1. **DRAM-first policy** — the original heap tries IRAM first for `32BIT`;
   MxR keeps IRAM as a fallback to reduce IRAM fragmentation.
2. **Descriptor limit** — MxR has a hard cap on simultaneous allocations.
   Monitor `alloc_fail_table_full` and raise `MXR_MAX_DESC` if needed.
3. **`realloc(ptr, 0)`** — frees the block and returns `NULL`
   (configurable via `MXR_REALLOC_ZERO_FREES`).

---

## Performance Considerations

All allocation operations run under `vPortETSIntrLock()` (interrupts
disabled). The hot path (`malloc`/`free`) is placed in **IRAM** by default.

Worst-case latency per operation:

```text
malloc:  O(N) free-block scan + O(N) descriptor shift
free:    O(log N) binary search + O(N) descriptor shift
```

where N = number of active descriptors.

| Scenario | Recommendation |
|----------|---------------|
| <= 256 active allocs | default settings are fine |
| 256-1024 active allocs | enable `MXR_SEARCH_BITMAP` |
| > 1024 active allocs | increase `MXR_MAX_DESC`, use bitmap |
| IRAM is tight | enable `MXR_IRAM_HOT_PATH_DISABLED` |

---

## Diagnostics

```c
typedef struct {
    bool     initialized;
    uint8_t  region_count;
    uint16_t desc_capacity;          /* CONFIG_MXR_MAX_DESC */
    uint16_t active_allocs;          /* current descriptor count */
    uint16_t max_active_allocs;      /* peak descriptor count */
    size_t   total_bytes;            /* DRAM + IRAM total */
    size_t   free_bytes;             /* DRAM + IRAM free */
    size_t   min_free_bytes;         /* historical low watermark */
    size_t   largest_free_block_bytes;
    size_t   iram_total_bytes;
    size_t   iram_free_bytes;
    size_t   iram_min_free_bytes;
    uint32_t exec_allocs;            /* EXEC allocations from IRAM */
    uint32_t iram_fallback_allocs;   /* 32BIT fallback allocations to IRAM */
    uint32_t cross_region_allocs;    /* cross-region DRAM fallback count */
    uint32_t alloc_fail_no_memory;
    uint32_t alloc_fail_table_full;
    uint32_t invalid_free_attempts;  /* double-free / wild pointer */
} mxr_status_t;
```

`mxr_dump()` prints the full arena state: totals, per-region statistics, IRAM
state, fallback counters and every live descriptor.

---

## Project Structure

```text
mxr_heap/
|-- CMakeLists.txt          # build config + linker --wrap flags
|-- Kconfig.projbuild       # menuconfig options
|-- include/
|   `-- mxr_malloc.h        # public API + descriptor helpers
|-- mxr_malloc.c            # core allocator
`-- mxr_heap_wrap.c         # linker --wrap integration (default mode)
```

Alternative integration files (`mxr_heap_compat.c`, `mxr_heap_port.c`) are
provided for replacement mode and must **never** be compiled together with the
wrap layer.

---

## FAQ

**Q: Why does `esp_get_free_heap_size()` differ from the original heap?**

The original heap reports DRAM + IRAM and tries IRAM first. With
`CONFIG_MXR_USE_IRAM=y`, MxR also reports DRAM + IRAM for `32BIT` queries.
Compare apples-to-apples with `heap_caps_get_dram_free_size()` (DRAM only).

**Q: Can I use `CONFIG_HEAP_TRACING` with MxR?**

No. Disable it. A Kconfig warning is emitted if it is enabled.

**Q: What happens if the descriptor table fills up?**

`malloc` returns `NULL` and `alloc_fail_table_full` is incremented, even if
memory is free. Increase `CONFIG_MXR_MAX_DESC`.

**Q: Is MxR safe to call from ISRs?**

No. Like the original ESP8266 heap, do not call `malloc`/`free` from
interrupt handlers.

**Q: Can I use MxR on ESP32?**

No. The memory map, linker symbols (`_bss_end`, `_iram_end`) and locking
primitives are ESP8266-specific.

**Q: How do I tune regions for my project?**

Run with defaults, call `mxr_dump()`, and watch per-region utilization and
`iram_fallback_allocs`. Adjust `MXR_REGION_SIZES` / `MXR_REGION_PERCENTS`
until fallbacks are rare and no region is starved.

**Q: What is cross-region fallback?**

When enabled (`CONFIG_MXR_CROSS_REGION_FALLBACK=y`), if a block's own
size-class region and IRAM are both exhausted, the allocator tries other
DRAM regions. It picks the region with the smallest `min_units` to minimize
fragmentation of large-block regions.

Disabled by default because it causes fragmentation. Monitor
`cross_region_allocs` — if it grows, increase the own region's percent in
`MXR_REGION_PERCENTS` or adjust `MXR_REGION_SIZES`.

**Q: What is `MXR_REALLOC_ZERO_FREES`?**

Controls `realloc(ptr, 0)` behavior:

| Value | Behavior |
|-------|----------|
| `1` (default) | `free(ptr)`, return `NULL` (glibc / ESP-IDF style) |
| `0` | allocate a minimal 4-byte block |

Define it in your project before including `mxr_malloc.h`:

```c
#define MXR_REALLOC_ZERO_FREES 0
#include "mxr_malloc.h"
```

---

## License

MIT License — see [LICENSE](LICENSE) for details.

---

<p align="center">
  <sub>Built for ESP8266 · Xtensa LX106 · FreeRTOS</sub>
</p>