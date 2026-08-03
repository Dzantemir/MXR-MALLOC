# MxR-malloc v2

<p align="center">
  <img src="https://img.shields.io/badge/ESP8266-RTOS%20SDK%20v3.4-blue" alt="ESP8266 RTOS SDK">
  <img src="https://img.shields.io/badge/language-C99-green" alt="C99">
  <img src="https://img.shields.io/badge/license-MIT-yellow" alt="License">
  <img src="https://img.shields.io/badge/platform-Xtensa%20LX106-orange" alt="Platform">
  <img src="https://img.shields.io/badge/descriptor-8%20bytes-5fd6a0" alt="8-byte descriptor">
</p>

<p align="center">
  <b>A descriptor-based memory allocator for **ESP8266 RTOS SDK**, engineered as a drop-in replacement for the standard `heap_caps` allocator.</b><br>
  Instead of embedding linked-list metadata inside every allocated block, MxR keeps a compact, sorted **descriptor table** outside the heap. Combined with size-class **regions**, an optional **IRAM heap**, and a configurable **fallback chain**, this yields lower fragmentation, zero per-block overhead, and full `heap_caps_*` API compatibility through linker `--wrap`.
</p>

<p align="center">
  <b>v2 highlights:</b> 8-byte descriptors with byte-granular offsets/lengths, IRAM-first routing for <code>MALLOC_CAP_32BIT</code>, configurable descriptor placement (DRAM / IRAM .text / IRAM .bss), compact type widths for ESP8266, and a selectable diagnostics verbosity.
</p>

---

## Table of Contents

- [Why MxR-malloc](#why-mxr-malloc)
- [Memory Layout](#memory-layout)
- [The Descriptor Table](#the-descriptor-table)
- [Allocation Flow (`malloc`)](#allocation-flow-malloc)
- [Free Flow (`free`)](#free-flow-free)
- [Reallocation (`realloc`)](#reallocation-realloc)
- [IRAM Heap](#iram-heap)
- [Cross-Region Fallback](#cross-region-fallback)
- [Free-Block Search Strategy](#free-block-search-strategy)
- [Initialization](#initialization)
- [Configuration](#configuration)
- [Integration Methods](#integration-methods)
- [API Reference](#api-reference)
- [Diagnostics](#diagnostics)
- [Performance Characteristics](#performance-characteristics)
- [Limitations](#limitations)
- [Project Structure](#project-structure)
- [License](#license)

---

## Why MxR-malloc

The core is intentionally small: a sorted descriptor table, a handful of
region descriptors, and the allocator state. All operations run under a
single ETS interrupt lock, which makes the allocator safe from both tasks
and ISRs (note: malloc/free still must not be called from ISRs — same rule
as the original heap).

---

## Memory Layout

MxR manages up to two physical arenas:

- **DRAM** — the main heap, from `_bss_end` up to `0x40000000`.
- **IRAM** — optional, from `_iram_end` up to `0x40100000 + CONFIG_SOC_IRAM_SIZE`.

All offsets and lengths are stored in **bytes** (4-byte aligned). The 31-bit
offset/length fields cover up to ~2 GB per arena — far beyond what ESP8266
physically has, so the format is portable to ESP32-class targets as well.

### DRAM arena with regions

The DRAM arena is divided into **regions**, each serving a range of block
sizes (a size class). The last region is always unlimited and absorbs
leftover memory.

```
DRAM arena (example: 3 regions, 80 KB total)
┌──────────────────────────────────────────────────────────────────────┐
│  Region 0          │  Region 1           │  Region 2                │
│  small blocks      │  medium blocks      │  large / unlimited       │
│  4–132 B           │  132–1024 B         │  1024 B – unlimited      │
│  15% of arena      │  45% of arena       │  remaining (40%)         │
├────────────────────┼─────────────────────┼──────────────────────────┤
│ start_byte = 0     │ start_byte = 12000  │ start_byte = 48000       │
│ min_bytes = 4      │ min_bytes = 132     │ min_bytes = 1024         │
│ max_bytes = 132    │ max_bytes = 1024    │ max_bytes = 0 (unlimited)│
└──────────────────────────────────────────────────────────────────────┘
        ▲                     ▲                       ▲
        └─────────────────────┴───────────────────────┘
              Free blocks are found by scanning gaps
              between descriptors *within* each region.
```

Each region tracks its own free space, low-water mark, and allocation count,
so diagnostics can report exactly which size class is under pressure.

### IRAM arena

IRAM is a single flat region used for:

- `MALLOC_CAP_EXEC` allocations (executable code), packed from the **start**.
- `MALLOC_CAP_32BIT` allocations (the IRAM-first path), packed from the **end**.

```
IRAM arena (low → high)
┌──────────────────────────────────────────────────────────────┐
│  EXEC blocks (from start)  │  free  │  32BIT blocks (from end) │
│  caps = 32BIT | EXEC       │        │  caps = 32BIT             │
└──────────────────────────────────────────────────────────────┘
        ▲                                  ▲             ▲
        │                                  └── reserve protects this side ──┘
        └── EXEC grows →                                          ← 32BIT grows
```

`CONFIG_MXR_IRAM_RESERVE_BYTES` keeps a gap between the two populations so
EXEC allocations never starve.

---

## The Descriptor Table

Every active allocation is represented by a single **8-byte descriptor**.
Nothing is stored inside the allocated block itself.

### Descriptor format

```
 mxr_desc_t (8 bytes)
 ┌─────────────────────────────────────┬─────────────────────────────────────┐
 │        off_flags (uint32_t)         │        len_flags (uint32_t)          │
 ├───────────────┬─────────────────────┼───────────────┬─────────────────────┤
 │ bit 31        │ bits 30..0          │ bit 31        │ bits 30..0          │
 │ IRAM flag     │ offset              │ EXEC flag     │ length              │
 │ 0=DRAM 1=IRAM │ in bytes (4-aligned)│ 1=executable  │ in bytes (4-aligned)│
 └───────────────┴─────────────────────┴───────────────┴─────────────────────┘
```

- **Offset** and **length** are stored directly in **bytes**, always 4-byte aligned.
- 31-bit fields cover up to ~2 GB per arena (`MXR_MAX_OFFSET_BYTES`, `MXR_MAX_LEN_BYTES`).
- The IRAM bit (bit 31 of `off_flags`) separates the two arenas inside one table.
- The EXEC bit (bit 31 of `len_flags`) marks executable IRAM blocks.
- `_Static_assert(sizeof(mxr_desc_t) == 8)` enforces the layout at build time.

### Descriptor table placement (new in v2)

The descriptor table can live in DRAM or IRAM, selected at build time:

| Kconfig option                | Section        | Linker patch | In binary | Use when                          |
| ---                           | ---            | ---          | ---       | ---                               |
| `MXR_DESC_IN_DRAM` (default)  | `.bss`         | not needed   | no        | safest, default                   |
| `MXR_DESC_IN_IRAM_TEXT`       | `.iram0.text`  | not needed   | yes (PROGBITS) | descriptor access must survive flash cache off |
| `MXR_DESC_IN_IRAM_BSS`        | `.iram0.bss`   | required (`*(.iram0.bss .iram0.bss.*)` wildcard in `esp8266.project.ld.in`) | no (NOLOAD) | same as above, but no flash cost |

All allocator state variables follow the same placement via `MXR_IRAM_DATA_ATTR`.

### Sorted order and binary search

The table is always sorted by `off_flags`. Because DRAM descriptors have the
IRAM bit cleared and IRAM descriptors have it set, **all DRAM descriptors
come first, all IRAM descriptors last**.

```
Descriptor table (sorted by off_flags)
┌────────┬────────┬────────┬────────┬────────┬────────┬────────┐
│ DRAM   │ DRAM   │ DRAM   │  ...   │ IRAM   │ IRAM   │ IRAM   │
│ off=0  │ off=40 │ off=96 │        │ off=8  │ off=64 │ off=200│
└────────┴────────┴────────┴────────┴────────┴────────┴────────┘
   ▲                                       ▲
   └── DRAM partition ──┘     IRAM partition ──┘
```

Lookup by offset is a binary search (`mxr_desc_find_key`). Insertion finds
its position with the same binary search, checks for overlaps with
neighbours, and shifts the tail. Removal shifts the tail left. With the
default capacity of 256 descriptors these operations are cheap; the
capacity is configurable from 16 up to 4096 (`CONFIG_MXR_MAX_DESC`).

---

## Allocation Flow (`malloc`)

`mxr_malloc_caps(size, caps)` is the heart of the allocator.

![Allocation flow (malloc) — flowchart](images/allocation-flow-malloc.svg)

### Step by step

1. **Validate and convert.** `size == 0` becomes 1. Sizes above
   `MXR_MAX_LEN_BYTES` (~2 GB) fail. The byte size is rounded up to 4-byte
   alignment.
2. **EXEC path.** If `caps` includes `MALLOC_CAP_EXEC`, the block must live
   in IRAM. MxR searches IRAM **from the start**
   (`mxr_iram_find_free_from_start`) and inserts an EXEC descriptor. This
   keeps EXEC blocks packed at the low end of IRAM.
3. **IRAM-first for plain 32BIT (new in v2).** For `MALLOC_CAP_32BIT` (or
   `caps == 0`) — i.e. what `malloc` / `calloc` / `zalloc` / `realloc`
   use — MxR tries IRAM **first**, allocating from the **end**
   (`mxr_iram_find_free_from_end`). This matches the original ESP8266 SDK
   behavior and frees DRAM for 8BIT/DMA consumers (i2c, i2s, spi, pwm, adc,
   wifi TX, lwIP pbuf). The path respects `CONFIG_MXR_IRAM_RESERVE_BYTES`
   and `CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES`.
4. **DRAM size-class lookup.** If IRAM cannot satisfy the request,
   `mxr_region_for_size` picks the DRAM region whose
   `[min_bytes, max_bytes]` range contains the block and whose capabilities
   match.
5. **Try the own region.** `mxr_try_alloc_region` searches for a
   contiguous gap via descriptor scan. On success a DRAM descriptor is
   inserted and the region's free counters are updated.
6. **Cross-region fallback (last resort).** If the own region cannot
   satisfy the request and `CONFIG_MXR_CROSS_REGION_FALLBACK=y`, MxR may
   place the block in another DRAM region.

### Routing summary

| Requested caps                                | Destination                                                       |
| ---                                           | ---                                                               |
| `EXEC` (optionally `\| 32BIT \| INTERNAL`)       | IRAM only, allocated from start                                   |
| `32BIT` (or `0`)                              | **IRAM first** (from end) → DRAM own region → cross-region        |
| `8BIT` / `DMA` / `SPIRAM`                     | DRAM only (never IRAM)                                            |

### Fallback chain

The order is fixed at compile time:

```
malloc_caps(size, caps)
   │
   ├── caps & EXEC  ──────────────────────►  IRAM (from start)  ──►  NULL
   │
   └── caps & (32BIT or 0)
         │
         ├── IRAM (from end, respect reserve)  ──►  success
         │
         ├── DRAM own size-class region        ──►  success
         │
         └── cross-region DRAM (opt-in)        ──►  NULL
```

![Fallback chain — flowchart](images/fallback-chain.svg)

> ℹ️ v1 had a configurable `CONFIG_MXR_CROSS_REGION_AFTER_IRAM` toggle that
> swapped the last two stages. v2 removes it: IRAM is always tried before
> cross-region, matching the original SDK's IRAM-first policy.

---

## Free Flow (`free`)

![Free flow (free) — flowchart](images/free-flow.svg)

Free is the mirror of malloc:

1. Determine the arena from the pointer address.
2. Convert the pointer to a byte offset and binary-search the descriptor.
3. Remove the descriptor (the gap it occupied becomes free automatically).
4. Return the bytes to the region (DRAM) or the IRAM pool.

There is **no coalescing step**. Because free space is derived from gaps
between descriptors, removing a descriptor instantly merges its space with
the neighbouring gaps.

Pointers that do not fall inside a managed arena, or that have no matching
descriptor, are counted in `invalid_free_attempts` and ignored — a double
free or wild pointer will not corrupt the heap.

---

## Reallocation (`realloc`)

`mxr_realloc_caps` tries hard to resize **in place** before falling back
to allocate-copy-free.

![Reallocation (realloc) — flowchart](images/reallocation-realloc.svg)

### `realloc(ptr, 0)` behavior (new in v2)

Controlled at compile time by the `MXR_REALLOC_ZERO_FREES` macro in
`mxr_malloc.h`:

- `1` (default, matches glibc / ESP-IDF): free `ptr` and return `NULL`.
- `0`: allocate a minimal 4-byte block instead of freeing.

### In-place rules (DRAM)

- **Same size** → return immediately.
- **Shrink** → shorten the descriptor and release the tail bytes.
- **Grow** → allowed only if the region still accepts the new size class,
  the capabilities still match, and the gap up to the next descriptor (or
  region end) is large enough.

If any condition fails, MxR allocates a fresh block, copies
`min(old, new)` bytes, and frees the original. The copy happens
**outside the lock**; the caller guarantees no concurrent access to `ptr`
during `realloc`.

### In-place rules (IRAM)

- An existing **EXEC** block always stays EXEC.
- A non-EXEC IRAM block can grow in place only while respecting the IRAM
  EXEC reserve (`CONFIG_MXR_IRAM_RESERVE_BYTES`) and the fallback size
  limit (`mxr_iram_can_grow_fallback`).
- Requesting EXEC on a non-EXEC block forces a move rather than silently
  converting the block.

---

## IRAM Heap

Enabled with `CONFIG_MXR_USE_IRAM` (on by default unless the SDK disables
IRAM). IRAM spans from `_iram_end` to the top of the IRAM window, subject
to the original SDK limits (512 B < size < 64 KB).

> ℹ️ In v2 the descriptor table may itself live in IRAM (`.iram0.text` or
> `.iram0.bss`); in that case `_iram_end` already accounts for the table,
> so the IRAM heap starts immediately after it.

### Capability routing (v2)

| Requested caps          | Destination                                                       |
| ---                     | ---                                                               |
| `EXEC`                  | IRAM only, allocated from start                                   |
| `DMA`, `8BIT`, `SPIRAM` | DRAM only (never IRAM)                                            |
| plain `32BIT` (or `0`)  | **IRAM first** (from end) → DRAM → cross-region                   |

The IRAM-first policy for `32BIT` is intentional: it mirrors the original
ESP8266 SDK allocator, which checked IRAM before DRAM. Keeping that order
means DRAM is preserved for byte-addressable / DMA-capable peripherals.

### EXEC reserve

To keep executable memory available, non-EXEC allocations into IRAM must
leave `CONFIG_MXR_IRAM_RESERVE_BYTES` free.
`CONFIG_MXR_IRAM_FALLBACK_MAX_BYTES` can additionally cap the size of any
single non-EXEC block (`0` = unlimited).

```
IRAM free space accounting
┌──────────────────────────────────────────────┐
│ EXEC used │ free usable by 32BIT │  reserved │
└──────────────────────────────────────────────┘
                                    ▲
                                    └─ kept free for EXEC allocations
```

---

## Cross-Region Fallback

A **last-resort** mechanism (`CONFIG_MXR_CROSS_REGION_FALLBACK`, off by
default). When a block's own size-class region is full and IRAM cannot
help, MxR may place the block in a *different* DRAM region, ignoring
size classes.

Selection strategy:

1. Skip the block's own region.
2. Among regions with matching caps and enough free space, prefer the one
   whose `min_bytes` is closest to the requested size.
3. Optionally pre-check the largest contiguous free block
   (`CONFIG_MXR_CROSS_REGION_CHECK_LARGEST`) to skip fragmented regions.
4. If allocation fails due to fragmentation, try the next candidate.

> ⚠️ This intentionally fragments large-block regions with small
> allocations. It exists to avoid allocation failure in edge cases, not
> for everyday use.

> ℹ️ v1 exposed `CONFIG_MXR_CROSS_REGION_AFTER_IRAM` to swap the order of
> the IRAM and cross-region stages. v2 removes that option: IRAM is
> always tried before cross-region.

---

## Free-Block Search Strategy

MxR uses a single strategy: **descriptor gap scan**.

> ℹ️ v1 also offered a bitmap-accelerated mode (`CONFIG_MXR_SEARCH_BITMAP`).
> v2 removes it — the descriptor scan is fast enough for typical embedded
> workloads and avoids carving bitmap memory out of the arena.

The scan walks the sorted DRAM descriptors and measures the gaps between
them:

```
region: [ ........................................ ]
descs:      ████      ██████        ███
gaps:    ^^^^    ^^^^^^      ^^^^^^^^    ^^^^^^^^^
              └─ first gap ≥ requested bytes wins
```

No extra memory is used. Cost is `O(n)` in the number of active DRAM
descriptors, which is small in typical embedded workloads. IRAM uses the
same strategy over its own slice of the descriptor table.

---

## Initialization

`mxr_init()` runs once (directly or via the wrapped `heap_caps_init`).

![Initialization — flowchart](images/initialization.svg)

Key points:

- The DRAM arena is bounded by the linker symbol `_bss_end` and `0x40000000`.
- All arena/region sizes are tracked in **bytes** (4-byte aligned).
- If the descriptor table is placed in IRAM (`.iram0.text` or
  `.iram0.bss`), `_iram_end` already accounts for it, so the IRAM heap
  starts immediately after the table.
- Region configuration is parsed from Kconfig strings (boundaries +
  weights). If parsing fails, MxR falls back to a single flat region so
  the system still boots.
- Compact type widths (`CONFIG_MXR_COMPACT_TYPES`, default on for ESP8266)
  use `uint16_t` for caps / min_bytes / max_bytes / alloc_count and
  `uint32_t` for offsets / sizes. Disable it for ESP32-class targets or
  when a size class needs to exceed 64 KB.

---

## Configuration

All options live under `idf.py menuconfig → Component config → MxR-malloc`.

### Presets

| Preset             | Regions | Boundaries (bytes)   | Weights (%)    | Typical workload     |
| ---                | ---     | ---                  | ---            | ---                  |
| Custom             | user    | user                 | user           | manual configuration |
| Balanced           | 3       | `4,132,1024`         | `15,45,0`      | general purpose      |
| Minimal            | 2       | `4,256`              | `30,0`         | bare-metal, no WiFi  |
| WiFi Station       | 3       | `4,132,1500`         | `15,45,0`      | LWIP client          |
| WiFi AP            | 4       | `4,96,512,1500`      | `12,25,30,0`   | many clients         |
| TLS / HTTPS        | 4       | `4,132,1024,4096`    | `10,20,25,0`   | mbedtls              |
| Audio / Streaming  | 4       | `4,256,1024,4096`    | `10,25,25,0`   | ADPCM, I2S, TCP      |
| HTTP Server        | 4       | `4,132,512,2048`     | `12,25,30,0`   | esp_http_server      |
| Sensor / Low-power | 3       | `4,64,256`           | `20,30,0`      | sensor polling       |
| Logging / Debug    | 3       | `4,96,512`           | `25,40,0`      | heavy ESP_LOG usage  |

Every preset drives `MXR_REGIONS`, `MXR_REGION_SIZES`, and
`MXR_REGION_PERCENTS` through the same parser. A trailing `0` weight
means "take all remaining memory" for the last region.

### Custom configuration

```
Number of heap regions: 3
Lower block size boundaries (bytes): "4,132,1024"
Memory weights (%):                  "15,45,20"
```

- Boundaries are in bytes and must be strictly increasing after 4-byte
  alignment.
- Weights are percentages; the sum must be ≤ 100.
- A trailing weight of `0` means "take all remaining memory".
- With `CONFIG_MXR_COMPACT_TYPES=y`, each boundary must fit in `uint16_t`
  (≤ 65535).

### Single region mode

Set `Number of heap regions = 1` (Custom). The allocator uses one flat
DRAM region spanning the whole arena; boundary and weight fields are
hidden and ignored. Internally this calls the same single-region setup
used by the init-time fallback.

### Descriptor table placement (new in v2)

```
Descriptor table placement:
  (*) DRAM (.bss) - default, safest
  ( ) IRAM (.iram0.text) - no linker patch needed
  ( ) IRAM (.iram0.bss) - requires patched linker script
```

Choosing IRAM keeps descriptor access working while the flash cache is
off, at the cost of IRAM (and, for `.iram0.text`, flash space in the
binary).

### Compact types (new in v2)

```
[*] Compact types (uint16 where possible, for ESP8266)
```

Saves ~16 bytes per region descriptor. Disable for ESP32 or if any size
class needs to exceed 64 KB.

### IRAM options

```
Enable IRAM heap (EXEC + 32BIT fallback)
Reserve IRAM bytes for EXEC allocations: 2048
Maximum block size for IRAM fallback:    0   (0 = unlimited)
```

In v2 the IRAM heap is tried **first** for plain `MALLOC_CAP_32BIT`
allocations (matching the original SDK). The reserve and the per-block
cap protect EXEC territory.

### Fallback options

```
Enable cross-region DRAM fallback (last resort)
  Check largest free block before cross-region fallback
```

> ℹ️ v1's `CONFIG_MXR_CROSS_REGION_AFTER_IRAM` toggle is gone in v2. The
> order is now fixed: IRAM → DRAM own region → cross-region.

### IRAM hot path

```
Disable placing malloc/free hot path in IRAM
IRAM hot path scope:
  (*) Core (malloc/free only)
  ( ) Allocation family (malloc/free/calloc/zalloc/realloc)
```

Placing the hot path in IRAM keeps `malloc`/`free` callable while the
flash cache is disabled (during flash write/erase).

### Diagnostics output level (new in v2)

```
Diagnostics output level:
  ( ) Minimal - totals only
  (*) Normal - totals, regions and counters
  ( ) Full - normal plus all descriptors
```

`mxr_dump()` honours this choice at runtime.

---

## Integration Methods

MxR ships three mutually exclusive integration layers. **Use only one.**

### 1. Linker `--wrap` (recommended)

`mxr_heap_wrap.c` + the wrap flags in `CMakeLists.txt`. The original heap
component stays in the build; calls are redirected at link time:

```
Application calls:     _heap_caps_malloc(...)
Linker redirects to:   __wrap__heap_caps_malloc(...)
Which calls:           mxr_malloc_caps(...)
```

Base wraps are always on; query / default-pool / esp-system / libc wraps
are optional via Kconfig.

### 2. Direct replacement

`mxr_heap_compat.c` defines the `heap_caps_*` symbols directly. Use this
only if the original heap component is **not** compiled.

### 3. Direct libc

`mxr_heap_port.c` defines `malloc`/`free`/etc. directly. Rarely needed;
the wrap approach is safer.

> ⚠️ Never compile `mxr_heap_compat.c` or `mxr_heap_port.c` together
> with the wrap layer — they will conflict.

---

## API Reference

### Core

```c
void  mxr_init(void);
void *mxr_malloc(size_t size);
void  mxr_free(void *ptr);
void *mxr_calloc(size_t count, size_t size);
void *mxr_realloc(void *ptr, size_t size);
void *mxr_zalloc(size_t size);
```

### Capability-aware

```c
void *mxr_malloc_caps(size_t size, uint32_t caps);
void *mxr_calloc_caps(size_t count, size_t size, uint32_t caps);
void *mxr_realloc_caps(void *ptr, size_t newsize, uint32_t caps);
void *mxr_zalloc_caps(size_t size, uint32_t caps);
```

Capability flags match the SDK: `MALLOC_CAP_EXEC`, `MALLOC_CAP_32BIT`,
`MALLOC_CAP_8BIT`, `MALLOC_CAP_DMA`, `MALLOC_CAP_INTERNAL`,
`MALLOC_CAP_SPIRAM`.

### Diagnostics

```c
void   mxr_get_status(mxr_status_t *status);
bool   mxr_get_region_status(int region_index, mxr_region_status_t *status);
size_t mxr_get_free_size_caps(uint32_t caps);
size_t mxr_get_min_free_size_caps(uint32_t caps);
void   mxr_dump(void);
```

---

## Diagnostics

`mxr_dump()` prints global statistics, per-region status, IRAM state,
and — at the `Full` verbosity — a snapshot of every descriptor:

```
I mxr_malloc: init ok: base=0x3ffe8000 bytes=80000 desc=DRAM(.bss) 2048 bytes
I mxr_malloc: IRAM heap ok: base=0x4010a1b0 bytes=16304
I mxr_malloc: MxR dump: initialized=1
I mxr_malloc: search mode: descriptor (bytes)
I mxr_malloc: total=96304 free=94176 min_free=94176 largest=80000
I mxr_malloc: desc used=0/256 max_used=0 desc=DRAM 2048 bytes
I mxr_malloc: exec_allocs=0 iram_fallback=0 cross_region=0 cross_skip_frag=0
I mxr_malloc: region 0: caps=0x0000080e start=0     total=12000 min=4    max=132  free=12000 min_free=12000 largest=12000 alloc=0
I mxr_malloc: region 1: caps=0x0000080e start=12000 total=36000 min=132  max=1024 free=36000 min_free=36000 largest=36000 alloc=0
I mxr_malloc: region 2: caps=0x0000080e start=48000 total=32000 min=1024 max=0    free=32000 min_free=32000 largest=32000 alloc=0
I mxr_malloc: stats: fail_mem=0 fail_table=0 invalid_free=0 max_allocs=0
```

`max=0` means unlimited. All byte counters are in **bytes** (not units).
The counters `alloc_fail_no_memory`, `alloc_fail_table_full`, and
`invalid_free_attempts` help diagnose exhaustion and misuse;
`max_active_allocs` tracks the high-water mark of the descriptor table.

---

## Performance Characteristics

- **malloc / free hot path** can reside in IRAM, safe during flash
  operations.
- **Free-block search** is `O(n)` over active descriptors (gap scan). With
  the default cap of 256 descriptors this is a handful of cache-line-
  friendly comparisons.
- **Descriptor insert/remove** is `O(n)` due to the array shift, bounded
  by `CONFIG_MXR_MAX_DESC`.
- **No coalescing pass** is ever needed — merging is implicit.
- **IRAM-first routing** for `MALLOC_CAP_32BIT` keeps the DRAM regions
  free for byte-addressable / DMA consumers, which improves DMA
  allocation success under pressure.
- **Size-class regions** keep small blocks from fragmenting large-block
  space, reducing worst-case fragmentation in long-running devices.

---

## Limitations

- Maximum arena size: **~2 GB** (31-bit byte offset, far beyond ESP8266
  physical memory).
- Maximum single allocation: **~2 GB** (same field width).
- Maximum simultaneous allocations: configurable, default **256**,
  range 16..4096. Each descriptor is 8 bytes (256 → 2 KB table).
- With `CONFIG_MXR_COMPACT_TYPES=y`, size-class boundaries are limited to
  65535 bytes; disable the option to lift the cap.
- IRAM heap size is still constrained by the SDK: 512 B < size < 64 KB.
- IRAM is unavailable for `DMA`, `8BIT`, or `SPIRAM` capabilities.
- Not compatible with `CONFIG_HEAP_TRACING` (a build warning is emitted).
- Cross-region fallback, if enabled, can fragment large-block regions.
- Placing the descriptor table in `.iram0.bss` requires patching
  `esp8266.project.ld.in` to add the `*(.iram0.bss .iram0.bss.*)`
  wildcard.

---

## Project Structure

```
mxr_malloc/
├── include/
│   └── mxr_malloc.h          # Public API, 8-byte descriptor helpers, types
├── mxr_malloc.c              # Core allocator (init, malloc/free/realloc, dump)
├── mxr_heap_wrap.c           # Linker --wrap layer (recommended)
├── mxr_heap_compat.c         # Direct heap_caps replacement (alternative)
├── mxr_heap_port.c           # Direct libc replacement (alternative)
├── CMakeLists.txt            # Build config and --wrap flags
└── Kconfig.projbuild         # Menuconfig: presets, placement, compact types,
                              # IRAM, cross-region, hot path, diagnostics level
```

---

## License

Provided as-is for use with the ESP8266 RTOS SDK.
