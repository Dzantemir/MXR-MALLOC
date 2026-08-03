# MxR-malloc

**Custom region-based allocator for the ESP8266 RTOS SDK with zero per-block overhead,
an IRAM heap, capability-aware routing, and detailed diagnostics.**

![ESP8266](https://img.shields.io/badge/chip-ESP8266-blue)
![SDK](https://img.shields.io/badge/SDK-ESP8266%20RTOS%20SDK-orange)
![lang](https://img.shields.io/badge/language-C-brightgreen)
![mode](https://img.shields.io/badge/integration-linker%20--wrap-yellow)
![sim](https://img.shields.io/badge/simulator-interactive%20HTML-9cf)

---

## Table of Contents

- [What it is](#what-it-is)
- [Key features](#key-features)
- [Differences from the original heap](#differences-from-the-original-heap)
- [Architecture](#architecture)
- [Descriptor format](#descriptor-format)
- [Allocation order](#allocation-order)
- [Capability routing](#capability-routing)
- [Installation](#installation)
- [Kconfig: all options](#kconfig-all-options)
- [Region presets](#region-presets)
- [API](#api)
- [Diagnostics](#diagnostics)
- [realloc behavior](#realloc-behavior)
- [Performance](#performance)
- [Limitations](#limitations)
- [Interactive simulator](#interactive-simulator)
- [Component structure](#component-structure)

---

## What it is

**MxR-malloc** is a complete replacement for the standard `heap` component of the ESP8266 RTOS SDK.
Instead of free-block linked lists with headers stored inside the memory, MxR keeps an
**external descriptor table** (in IRAM or DRAM), while the heap itself stays "clean":
every byte of the arena is usable, blocks have no inline headers, which is critical for
DMA buffers (I2S, WiFi, lwIP pbuf).

The allocator is wired in through **linker `--wrap`**, so it requires no SDK patches:
all calls to `heap_caps_malloc`, `malloc`, `esp_get_free_heap_size`, etc. automatically
land in MxR.

## Key features

- **0 bytes overhead per block** — metadata lives in a separate descriptor table (8 B/descriptor).
- **Size-class regions** — up to 16 regions with boundaries and weights (in %), configured via Kconfig strings.
- **IRAM heap**:
  - `MALLOC_CAP_EXEC` → IRAM only;
  - pure `MALLOC_CAP_32BIT` → **IRAM-first** (as in the original SDK), freeing DRAM for 8BIT/DMA consumers;
  - configurable **reserve** for EXEC and a fallback-block size cap.
- **Capability-aware**: `EXEC / 32BIT / 8BIT / DMA / SPIRAM / INTERNAL`.
- **Cross-region fallback** (optional, last resort) — allocates in a "foreign" region when the own one is fragmented.
- **realloc in-place**: shrink/grow without copying, if the adjacent gap is free.
- **Hot path in IRAM** (`malloc/free`, optionally the whole family) — works with flash cache disabled.
- **Descriptor table** in DRAM, `.iram0.text`, or `.iram0.bss` (selectable).
- **Full diagnostics**: per-region status, min-free watermark, largest free block, fallback/error counters, dump of all descriptors.
- **Presets** for typical workloads: WiFi Station/AP, Audio, TLS, HTTP server, Sensor, Logging.

## Differences from the original heap

| Parameter | Original `heap` | **MxR-malloc** |
|---|---|---|
| Block metadata | inline `mem_blk_t` (8–20 B/block) | external descriptors, 8 B/block (in IRAM/DRAM) |
| Overhead per block | 8+ bytes | **0 bytes** |
| Free-block search | first-fit over a linked list | gap-scan over a sorted table (binary search on the key) |
| IRAM | separate region, first-fit | EXEC + managed 32BIT-fallback with reserve |
| Size classes | none | up to 16 regions |
| Statistics | free/min_free per region | regions + largest free + fallback/error counters |
| Heap tracing | supported | incompatible (see limitations) |
| Block alignment | 4 | 4 |

## Architecture

```
DRAM: _bss_end ───────────────────────────────────────────────► 0x40000000
┌───────────────┬───────────────────────┬────────────────────────────────────┐
│   REGION 0    │       REGION 1        │             REGION 2               │
│   4…255 B     │      256…1023 B       │        1024 B … unlimited          │
│   percent 10% │       percent 25%     │   percent 25% + all remaining      │
└───────────────┴───────────────────────┴────────────────────────────────────┘
   ▲ blocks found by gap-scan between descriptors within a region

IRAM: _iram_end ─────────────────────────► 0x40100000 + SOC_IRAM_SIZE
┌──────────────────────────────────────────────────────────────┬─────────────┐
│  EXEC blocks (from start)    32BIT-fallback (from end)       │  reserve    │
└──────────────────────────────────────────────────────────────┴─────────────┘

Descriptor table (sorted by the off_flags key):
┌──────────┬──────────┬──────────┬──────────┐
│ DRAM blk │ DRAM blk │   ...    │ IRAM blk │   (IRAM descriptors always at the end)
└──────────┴──────────┴──────────┴──────────┘
```

## Descriptor format

Each descriptor is exactly **8 bytes**:

```
off_flags (uint32):
  bit 31      = MXR_OFF_FLAG_IRAM  (0 = DRAM, 1 = IRAM)
  bits 30..0  = offset in bytes from the arena base

len_flags (uint32):
  bit 31      = MXR_LEN_FLAG_EXEC  (1 = executable block)
  bits 30..0  = length in bytes (aligned to 4)
```

The table is sorted by `off_flags`; lookup/insertion/deletion use binary search.
Free gaps are computed as the gap between adjacent descriptors.

## Allocation order

For `mxr_malloc_caps_locked(size, caps)`:

1. **`MALLOC_CAP_EXEC`** → IRAM only, free-block search **from the start**. Never lands in DRAM.
2. **Pure `MALLOC_CAP_32BIT`** (without `8BIT/DMA/SPIRAM`) → **IRAM-first**:
   - if IRAM is enabled, there is enough free space accounting for `IRAM_RESERVE_BYTES`
     and the block is no larger than `IRAM_FALLBACK_MAX_BYTES` → placed in IRAM **from the end**;
   - otherwise — in DRAM.
3. **DRAM**: pick a region by size class (`min_bytes ≤ size ≤ max_bytes`) and caps,
   search for a free gap **from the start of the region**.
4. **Cross-region fallback** (if enabled): iterate the remaining DRAM regions
   starting from the closest size class, until space is found.
5. On failure → increment `alloc_fail_no_memory` / `alloc_fail_table_full`, return `NULL`.

`8BIT`, `DMA`, `SPIRAM` are **never** placed in IRAM.

## Capability routing

| Requested caps | DRAM regions | IRAM |
|---|---|---|
| `EXEC` | — | ✅ IRAM only (from start) |
| `8BIT` / `DMA` / `SPIRAM` (any combination) | ✅ DRAM only | ❌ disallowed |
| pure `32BIT` (`malloc`, `calloc`, `zalloc`, `realloc`) | ✅ if IRAM is busy/small | ✅ **IRAM-first** (fallback from end) |
| `32BIT + INTERNAL` | ✅ | ✅ |

## Installation

1. Copy the component into your project:

```
components/
└── mxr_malloc/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild
    ├── mxr_malloc.c
    ├── mxr_malloc.h
    └── mxr_heap_wrap.c
```

2. The component uses **wrap mode** and adds the linker flags itself:

```cmake
-Wl,--wrap=heap_caps_init
-Wl,--wrap=_heap_caps_malloc
-Wl,--wrap=_heap_caps_free
-Wl,--wrap=_heap_caps_realloc
-Wl,--wrap=_heap_caps_calloc
-Wl,--wrap=_heap_caps_zalloc
# optional: heap query / default pool / esp_system / libc
```

3. **Do not compile** `mxr_heap_compat.c` and `mxr_heap_port.c` in wrap mode —
   they conflict with the original `heap` component.
4. `idf.py menuconfig` → **MxR-malloc** → choose a preset or Custom.
5. Build and flash as usual.

> ⚠️ Disable `CONFIG_HEAP_TRACING` — it is incompatible with MxR wrap mode
> (Kconfig will warn automatically).

## Kconfig: all options

### Core

| Option | Default | Description |
|---|---|---|
| `MXR_PRESET_*` | `CUSTOM` | Region configuration preset |
| `MXR_MAX_DESC` | `256` | Max number of simultaneous allocations (8 B/descriptor) |
| `MXR_DESC_IN_DRAM / _IRAM_TEXT / _IRAM_BSS` | `DRAM` | Where to store the descriptor table |
| `MXR_COMPACT_TYPES` | `y` | `uint16` for size classes/counters (ESP8266) |
| `MXR_IRAM_HOT_PATH_DISABLED` | `n` | Do not place the hot path in IRAM |
| `MXR_IRAM_PATH_CORE / _ALLOC_FAMILY` | `CORE` | What to keep in IRAM: just malloc/free or the whole family |

### IRAM heap

| Option | Default | Description |
|---|---|---|
| `MXR_USE_IRAM` | `y` | Enable the IRAM heap (EXEC + 32BIT fallback) |
| `MXR_IRAM_RESERVE_BYTES` | `2048` | IRAM reserve for EXEC allocations |
| `MXR_IRAM_FALLBACK_MAX_BYTES` | `0` (∞) | Max size of a non-EXEC block going to IRAM |

### Regions

| Option | Description |
|---|---|
| `MXR_REGIONS` | Number of regions (1–16). `1` = flat arena |
| `MXR_REGION_SIZES` | CSV of size-class lower bounds, in bytes: `"4,256,1024,4096"` |
| `MXR_REGION_PERCENTS` | CSV of region weights, sum ≤ 100; a trailing `0` = takes the remainder |

Rules:
- there must be exactly `MXR_REGIONS` boundaries, and the same number of percents;
- boundaries must be strictly increasing and aligned to 4;
- only the last region may be unlimited;
- a region cannot be smaller than its lower bound.

### Fallback and integration

| Option | Default | Description |
|---|---|---|
| `MXR_CROSS_REGION_FALLBACK` | `n` | Allow allocation in a foreign region (fragments large regions!) |
| `MXR_CROSS_REGION_CHECK_LARGEST` | `n` | Check largest free before cross-region |
| `MXR_WRAP_HEAP_QUERY` | `y` | wrap `heap_caps_get_free_size/minimum/dram` |
| `MXR_WRAP_DEFAULT_POOL` | `y` | wrap `heap_caps_malloc_default/realloc_default` |
| `MXR_WRAP_ESP_SYSTEM` | `y` | wrap `esp_get_free_heap_size` and others |
| `MXR_WRAP_LIBC` | `n` | wrap `malloc/free/calloc/realloc/zalloc` |

### Diagnostics

| Level | Output |
|---|---|
| `MXR_DUMP_MINIMAL` | total / free / min_free / largest |
| `MXR_DUMP_NORMAL` | + search mode, descriptors, counters, IRAM, regions |
| `MXR_DUMP_FULL` | + dump of every active descriptor |

## Region presets

| Preset | Regions | `MXR_REGION_SIZES` | `MXR_REGION_PERCENTS` |
|---|---|---|---|
| Balanced | 3 | `4,132,1024` | `15,45,0` |
| Minimal | 2 | `4,256` | `30,0` |
| WiFi Station | 3 | `4,132,1500` | `15,45,0` |
| WiFi AP | 4 | `4,96,512,1500` | `12,25,30,0` |
| TLS/HTTPS | 4 | `4,132,1024,4096` | `10,20,25,0` |
| **Audio/Streaming** | 4 | `4,256,1024,4096` | `10,25,25,0` |
| HTTP Server | 4 | `4,132,512,2048` | `12,25,30,0` |
| Sensor | 3 | `4,64,256` | `20,30,0` |
| Logging | 3 | `4,96,512` | `25,40,0` |

> A trailing weight of 0 means "take all remaining memory" — that way a large region
> automatically gets all the free memory.

## API

### SDK compatibility (replaced via `--wrap`)

```c
void  *heap_caps_malloc(size_t size, uint32_t caps);      // → mxr_malloc_caps
void   heap_caps_free(void *ptr);                          // → mxr_free
void  *heap_caps_calloc(size_t n, size_t sz, uint32_t caps);
void  *heap_caps_realloc(void *p, size_t sz, uint32_t caps);
void  *heap_caps_zalloc(size_t sz, uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_dram_free_size(void);
```

### MxR native API

```c
void   mxr_init(void);                      // called from heap_caps_init
void  *mxr_malloc(size_t size);             // == caps MALLOC_CAP_32BIT
void  *mxr_malloc_caps(size_t size, uint32_t caps);
void  *mxr_calloc(size_t count, size_t size);
void  *mxr_calloc_caps(size_t count, size_t size, uint32_t caps);
void  *mxr_zalloc(size_t size);
void  *mxr_zalloc_caps(size_t size, uint32_t caps);
void  *mxr_realloc(void *ptr, size_t newsize);
void  *mxr_realloc_caps(void *ptr, size_t newsize, uint32_t caps);
void   mxr_free(void *ptr);

void   mxr_get_status(mxr_status_t *status);
bool   mxr_get_region_status(int region_index, mxr_region_status_t *status);
size_t mxr_get_free_size_caps(uint32_t caps);
size_t mxr_get_min_free_size_caps(uint32_t caps);
void   mxr_dump(void);
```

### Status structures

```c
typedef struct {
    bool     initialized;
    uint8_t  region_count;
    uint16_t desc_capacity;          // CONFIG_MXR_MAX_DESC
    uint16_t active_allocs;          // currently live
    uint16_t max_active_allocs;      // peak
    size_t   total_bytes, free_bytes, min_free_bytes;
    size_t   largest_free_block_bytes;
    size_t   iram_total_bytes, iram_free_bytes, iram_min_free_bytes;
    uint32_t exec_allocs, iram_fallback_allocs, cross_region_allocs;
    uint32_t alloc_fail_no_memory, alloc_fail_table_full, invalid_free_attempts;
    uint32_t cross_region_skip_fragmented;
} mxr_status_t;
```

## Diagnostics

`mxr_dump()` at the NORMAL level:

```
I mxr_malloc: total=67456 free=41200 min_free=38112 largest=20480
I mxr_malloc: search mode: descriptor (bytes)
I mxr_malloc: desc used=37/256 max_used=58 desc=DRAM(.bss) 2048 bytes
I mxr_malloc: exec_allocs=2 iram_fallback=114 cross_region=0 cross_skip_frag=0
I mxr_malloc: IRAM: base=0x40108a40 total=18304 free=12288 min_free=10112
I mxr_malloc: region 0: caps=0x0000080e start=0 total=7168 min=4 max=255 free=3072 ...
I mxr_malloc: region 1: caps=0x0000080e start=7168 total=17920 min=256 max=1023 free=9216 ...
I mxr_malloc: region 2: caps=0x0000080e start=25088 total=42368 min=1024 max=0 free=28912 ...
I mxr_malloc: stats: fail_mem=0 fail_table=0 invalid_free=0 max_allocs=58
```

What to watch:
- `min_free` — the watermark, the main headroom indicator;
- `largest` — the largest contiguous chunk (important for big buffers);
- `fail_table` — you hit the `MXR_MAX_DESC` cap, raise it;
- `cross_region > 0` — regions are sized inaccurately, a size class is leaking into others.

## realloc behavior

- `realloc(p, 0)` → `free(p)`, returns `NULL` (compatible with glibc/ESP-IDF,
  toggled by `MXR_REALLOC_ZERO_FREES`).
- **Shrink** — in place, the surplus is returned to the region.
- **Grow** — in place, if there is enough room up to the next descriptor / region boundary.
- Otherwise — allocate + copy + free (copying outside the spinlock).
- An EXEC block never turns into a non-EXEC one; a non-EXEC IRAM block does not grow
  at the expense of `IRAM_RESERVE_BYTES`.

## Performance

- The `malloc/free` hot path is placed in **IRAM** — allocations are safe during
  flash operations (when the cache is disabled).
- No inline headers → no cache-line pollution with metadata, DMA buffers
  use 100% of their capacity.
- Descriptor lookup is **O(log n)**, insertion with shift is O(n), gap-scan is O(n)
  over the number of live blocks (typically tens).
- IRAM-first for `MALLOC_CAP_32BIT` offloads DRAM for 8BIT/DMA consumers
  (I2S DMA, lwIP pbuf, WiFi TX) — less fragmentation of critical memory.

## Limitations

- At most `CONFIG_MXR_MAX_DESC` live allocations (default 256).
- `CONFIG_HEAP_TRACING` is incompatible (wrap conflict).
- All blocks are aligned to 4 bytes.
- `MXR_DESC_IN_IRAM_BSS` requires patching the linker script (`*(.iram0.bss .iram0.bss.*)`).
- Cross-region fallback is off by default: it saves you from OOM, but fragments
  large regions with small allocations.

## Interactive simulator

The repo ships a **`simulator.html`** — open it in any browser, no build required.

Features:
- full configuration: DRAM/IRAM size, regions (sizes + percents),
  IRAM reserve, fallback max, cross-region, descriptor cap, IRAM-first on/off;
- one-click region presets from Kconfig;
- workload generators: WiFi Station, Audio/I2S streaming, TLS burst, Sensor,
  Logging, Chaos, manual mode;
- manual operations: malloc with caps selection (`32BIT/8BIT/DMA/EXEC`), realloc and free
  by clicking a block;
- real-time: a DRAM-region map and an IRAM map, colors by block type
  (EXEC / IRAM-fallback / cross-region / DMA-8BIT / 32BIT), descriptor table;
- charts of free/min-free, all `mxr_status_t` counters, an event log.

This is handy for tuning `MXR_REGION_SIZES`/`MXR_REGION_PERCENTS` for your workload
before flashing to hardware.

## Component structure

```
mxr_malloc/
├── CMakeLists.txt        # linker wrap flags
├── Kconfig.projbuild     # presets and options
├── mxr_malloc.h          # API, types, descriptor bit format
├── mxr_malloc.c          # allocator core
├── mxr_heap_wrap.c       # __wrap_* functions (used in wrap mode)
├── mxr_heap_compat.c     # direct heap replacement (NOT in wrap mode)
└── mxr_heap_port.c       # direct libc wrappers (NOT in wrap mode)
```

## License

MIT
