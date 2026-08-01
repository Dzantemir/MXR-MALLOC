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



## Table of Contents

* [Overview](#overview)
* [Features](#features)
* [Architecture](#architecture)
* [Memory Layout](#memory-layout)
* [Allocation Policy](#allocation-policy)
* [Descriptor Format](#descriptor-format)
* [Region Configuration](#region-configuration)
* [Search Modes](#search-modes)
* [IRAM Support](#iram-support)
* [Cross-Region Fallback](#cross-region-fallback)
* [Installation](#installation)
* [Configuration](#configuration)
* [API Reference](#api-reference)
* [Usage Examples](#usage-examples)
* [Comparison with Original Heap](#comparison-with-original-heap)
* [Performance Considerations](#performance-considerations)
* [Diagnostics](#diagnostics)
* [Project Structure](#project-structure)
* [FAQ](#faq)
* [License](#license)



## Overview

**MxR-malloc** is a drop-in replacement for the default ESP8266 RTOS SDK heap
allocator. Instead of in-band block headers it keeps all metadata in an
**out-of-band descriptor table**, giving exact 4-byte alignment with zero
per-block overhead inside the arena.

The allocator manages two physical arenas:

|Arena|Address Range|Capabilities|
|-|-|-|
|**DRAM**|`\_bss\_end` -> `0x40000000`|`8BIT` / `32BIT` / `DMA` / `INTERNAL`|
|**IRAM**|`\_iram\_end` -> `0x40100000 + SOC\_IRAM\_SIZE`|`32BIT` / `EXEC`|

Ordinary allocations go to **DRAM first**. When DRAM is exhausted, 32-bit
allocations **fall back to IRAM**. Executable memory (`MALLOC\_CAP\_EXEC`) is
served **exclusively from IRAM**.



## Features

* **Zero in-band overhead** — metadata lives in a separate descriptor table.
* **Exact 4-byte alignment** — every allocation is naturally aligned.
* **Size-class regions** — DRAM is split into configurable regions by block size.
* **IRAM fallback** — unused IRAM is reclaimed as a 32-bit fallback pool.
* **Cross-region DRAM fallback (opt-in)** — when a size-class region and IRAM
are both exhausted, allocation can spill into other DRAM regions as a last
resort (disabled by default to avoid fragmentation).
* **EXEC support** — `MALLOC\_CAP\_EXEC` allocations come from IRAM only.
* **Two search modes** — descriptor gap search (default) or bitmap search
(carved from the arena, no fixed 4 KB cost).
* **Linker `--wrap` integration** — replaces the heap without touching the SDK.
* **Full Kconfig integration** — every parameter is set via `menuconfig`.
* **Rich diagnostics** — per-region stats, fallback counters, heap dump.
* **IRAM-safe hot path** — `malloc`/`free` are placed in IRAM by default;
`calloc`/`zalloc`/`realloc` can optionally join them via
`CONFIG\_MXR\_IRAM\_PATH\_ALLOC\_FAMILY`.
* **Word-wise copy/clear** — `realloc`/`calloc`/`zalloc` never emit byte stores
into IRAM (IRAM is 32-bit access only).



## Architecture

```text
+-------------------------------------------------------------+
|                    Application / SDK                        |
|         malloc / calloc / realloc / heap\_caps\_malloc        |
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
heap\_caps\_malloc(size, caps)
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
    |             |  \[ bitmap (if enabled, carved) ]   |
```

### IRAM arena

```text
0x40100000                              0x40100000 + SOC\_IRAM\_SIZE
    |  .iram.text |      IRAM heap arena      |
    |<----------->|<------------------------->|
    |   (code)    |  EXEC blocks grow -->     |
    |             |  <-- fallback blocks grow |
    |             |  \[ reserve for EXEC ]     |
```



## Allocation Policy

|Request|Destination|Notes|
|-|-|-|
|`malloc(size)`|DRAM -> IRAM fallback|equivalent to `MALLOC\_CAP\_32BIT`|
|`MALLOC\_CAP\_32BIT`|DRAM -> IRAM fallback|ordinary 32-bit memory|
|`MALLOC\_CAP\_8BIT`|DRAM only|byte-accessible memory|
|`MALLOC\_CAP\_DMA`|DRAM only|DMA-capable memory|
|`MALLOC\_CAP\_INTERNAL`|DRAM only|internal RAM|
|`MALLOC\_CAP\_EXEC`|IRAM only|executable memory|
|`MALLOC\_CAP\_EXEC \| 32BIT`|IRAM only|executable 32-bit memory|
|Cross-region fallback|Other DRAM regions|Only if `CONFIG\_MXR\_CROSS\_REGION\_FALLBACK=y` and own region + IRAM exhausted|
|`MALLOC\_CAP\_SPIRAM`|`NULL`|not supported on ESP8266|

### Fallback chain (32BIT / default)

```text
1. Own DRAM size-class region
2. IRAM fallback (if CONFIG\_MXR\_USE\_IRAM=y)
3. Cross-region DRAM fallback (if CONFIG\_MXR\_CROSS\_REGION\_FALLBACK=y)
4. NULL
```

### IRAM fallback rules

```text
IRAM fallback is allowed when ALL of:
  - CONFIG\_MXR\_USE\_IRAM = y
  - request does NOT include EXEC, DMA, 8BIT or SPIRAM
  - request includes 32BIT (or caps == 0)
  - IRAM free >= requested + CONFIG\_MXR\_IRAM\_RESERVE\_BYTES
  - requested size <= CONFIG\_MXR\_IRAM\_FALLBACK\_MAX\_BYTES (0 = unlimited)
```



## Descriptor Format

Each live block owns one 4-byte descriptor:

```text
  off\_flags (16 bits)              len\_flags (16 bits)
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

|Parameter|Value|
|-|-|
|Max offset|32 767 units (131 068 bytes)|
|Max block length|32 768 units (131 072 bytes)|
|Max arena size|32 768 units (131 072 bytes)|
|Alignment|4 bytes|



## Region Configuration

DRAM regions are defined by their **lower boundaries only**, so gaps and
overlaps are impossible by construction. The last region is always unlimited.

```text
CONFIG\_MXR\_REGIONS=3
CONFIG\_MXR\_REGION\_SIZES="4,132,1024"
CONFIG\_MXR\_REGION\_PERCENTS="15,45,20"
```

creates:

|Region|Accepts|Memory|
|-|-|-|
|0|4 .. 128 B|15%|
|1|132 .. 1020 B|45%|
|2|1024 B .. max|20% + remainder|

If the percent sum is below 100, the remainder is added to the last region.
If the last percent is `0`, the last region receives all remaining memory.



## Search Modes

### Descriptor gap search (default)

```text
CONFIG\_MXR\_SEARCH\_DESCRIPTOR=y
```

Walks the sorted descriptor table and finds the first gap >= requested size.

* **Cost:** O(N) where N = active descriptors in the region
* **RAM:** 0 extra
* **Best for:** <= 512 active allocations

### Bitmap search (optional)

```text
CONFIG\_MXR\_SEARCH\_BITMAP=y
```

Maintains a 1-bit-per-unit bitmap **carved from the end of the DRAM arena** at
init time, so its size scales with the actual arena instead of a fixed 4 KB.

```text
Arena:  \[  allocatable units  ]\[ bitmap (carved) ]
         <--- s\_arena\_total --><--- s\_bitmap --->

For 80 KB DRAM:
  80 000 / 4 = 20 000 units
  20 000 / 8 = 2 500 bytes bitmap
```

* **Cost:** O(arena\_units / 32) word scans
* **RAM:** \~2.5 KB for an 80 KB arena (carved, not static)
* **Covers:** DRAM only — IRAM always uses descriptor search



## IRAM Support

```text
CONFIG\_MXR\_USE\_IRAM=y                    # default y if !CONFIG\_HEAP\_DISABLE\_IRAM
CONFIG\_MXR\_IRAM\_RESERVE\_BYTES=2048       # reserve for EXEC allocations
CONFIG\_MXR\_IRAM\_FALLBACK\_MAX\_BYTES=0     # 0 = unlimited fallback block size
```

* **EXEC allocations** grow from the **start** of IRAM.
* **Fallback allocations** grow from the **end** of IRAM.
* A **reserve** (default 2048 bytes) protects EXEC space from fallback.
* EXEC allocations ignore the reserve.
* IRAM is never used for `DMA` or `8BIT` requests.

Because IRAM on ESP8266 is 32-bit access only, the allocator uses word-wise
copy/clear (`mxr\_memcpy\_words` / `mxr\_memset\_words`) for any operation that may
touch IRAM, so `realloc`/`calloc`/`zalloc` never emit byte stores there.

### IRAM hot path scope

By default only `malloc`/`free` are IRAM-resident. The Kconfig choice
**IRAM hot path scope** selects which allocator functions live in IRAM:

|Choice|Functions in IRAM|Footprint|
|-|-|-|
|`MXR\_IRAM\_PATH\_CORE` (default)|`malloc` / `free`|smallest|
|`MXR\_IRAM\_PATH\_ALLOC\_FAMILY`|`malloc` / `free` / `calloc` / `zalloc` / `realloc`|larger|

The scope choice is active only when `CONFIG\_MXR\_IRAM\_HOT\_PATH\_DISABLED=n`.
Enable `MXR\_IRAM\_PATH\_ALLOC\_FAMILY` if allocations can run from contexts that
execute while flash cache is disabled (e.g. flash-write callbacks). Verify IRAM
usage with `idf.py size` after enabling.



## Cross-Region Fallback

**Disabled by default.** Enable only when you understand the fragmentation
trade-off.

```text
CONFIG\_MXR\_CROSS\_REGION\_FALLBACK=y
CONFIG\_MXR\_CROSS\_REGION\_AFTER\_IRAM=y     # default y
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
**smallest `min\_units`** that still has enough free space. This minimizes
damage: a 10-unit block prefers region 1 (min=33) over region 2 (min=256),
keeping region 2 intact for large buffers.

### Monitoring

Watch `cross\_region\_allocs` in `mxr\_get\_status()` / `mxr\_dump()`:

```text
cross\_region\_allocs=17   <- if this grows, increase own region percent
```

If the counter grows steadily, your `MXR\_REGION\_PERCENTS` are misconfigured —
the own region is chronically undersized.



## Installation

### 1\. Copy the component

```bash
cp -r mxr\_heap/ <your\_project>/components/mxr\_heap/
```

### 2\. Component structure

```text
components/mxr\_heap/
|-- CMakeLists.txt
|-- Kconfig.projbuild
|-- include/
|   `-- mxr\_malloc.h
|-- mxr\_malloc.c
`-- mxr\_heap\_wrap.c
```

### 3\. Configure

```bash
idf.py menuconfig
# -> MxR-malloc
```

### 4\. Build

```bash
idf.py fullclean
idf.py build
```

> Always run `fullclean` after changing MxR Kconfig options. Stale `sdkconfig`
> values can otherwise override the new defaults.



## Configuration

### Global

|Option|Type|Default|Description|
|-|-|-|-|
|`MXR\_MAX\_DESC`|int (16-4096)|`256`|Max simultaneous allocations (shared DRAM+IRAM)|
|`MXR\_IRAM\_HOT\_PATH\_DISABLED`|bool|`n`|Place malloc/free hot path in flash instead of IRAM|
|`MXR\_IRAM\_PATH\_CORE` / `MXR\_IRAM\_PATH\_ALLOC\_FAMILY`|choice|`CORE`|IRAM hot path scope: `malloc`/`free` only (CORE) or also `calloc`/`zalloc`/`realloc` (ALLOC\_FAMILY). Active only when `MXR\_IRAM\_HOT\_PATH\_DISABLED=n`|

### IRAM

|Option|Type|Default|Description|
|-|-|-|-|
|`MXR\_USE\_IRAM`|bool|`y`\*|Enable IRAM heap (EXEC + 32BIT fallback)|
|`MXR\_IRAM\_RESERVE\_BYTES`|int (0-32768)|`2048`|IRAM reserved for EXEC allocations|
|`MXR\_IRAM\_FALLBACK\_MAX\_BYTES`|int (0-65536)|`0`|Max non-EXEC block allowed into IRAM (0=inf)|

\* default `y` if `CONFIG\_HEAP\_DISABLE\_IRAM` is not set.

### Regions

|Option|Type|Default|Description|
|-|-|-|-|
|`MXR\_REGIONS`|int (2-16)|`3`|Total number of DRAM regions|
|`MXR\_REGION\_SIZES`|string|`"4,132,1024"`|Lower block-size boundaries (bytes)|
|`MXR\_REGION\_PERCENTS`|string|`"15,45,20"`|Memory weight per region (%)|

### Cross-region fallback

|Option|Type|Default|Description|
|-|-|-|-|
|`MXR\_CROSS\_REGION\_FALLBACK`|bool|`n`|Enable cross-region DRAM fallback (last resort)|
|`MXR\_CROSS\_REGION\_AFTER\_IRAM`|bool|`y`|Cross-region only after IRAM is exhausted|

### Search mode

|Option|Description|
|-|-|
|`MXR\_SEARCH\_DESCRIPTOR`|Descriptor gap search (default, 0 extra RAM)|
|`MXR\_SEARCH\_BITMAP`|Bitmap search (carved from arena, DRAM only)|

### Linker integration

|Option|Default|Wraps|
|-|-|-|
|`MXR\_WRAP\_HEAP\_QUERY`|`y`|`heap\_caps\_get\_free\_size`, `...\_minimum\_free\_size`, `...\_dram\_free\_size`|
|`MXR\_WRAP\_DEFAULT\_POOL`|`y`|`heap\_caps\_malloc\_default`, `heap\_caps\_realloc\_default`|
|`MXR\_WRAP\_ESP\_SYSTEM`|`y`|`esp\_get\_free\_heap\_size`, `...\_minimum`, `...\_internal`|
|`MXR\_WRAP\_LIBC`|`n`|`malloc`, `free`, `calloc`, `realloc`, `zalloc`|
|`MXR\_WARN\_HEAP\_TRACING`|`y`|Warn if `CONFIG\_HEAP\_TRACING` is enabled|



## API Reference

### Core API

```c
/\* lifecycle + plain API \*/
void  mxr\_init(void);
void \*mxr\_malloc(size\_t size);
void  mxr\_free(void \*ptr);
void \*mxr\_calloc(size\_t count, size\_t size);
void \*mxr\_realloc(void \*ptr, size\_t size);
void \*mxr\_zalloc(size\_t size);

/\* capability-aware API \*/
void \*mxr\_malloc\_caps(size\_t size, uint32\_t caps);
void \*mxr\_calloc\_caps(size\_t count, size\_t size, uint32\_t caps);
void \*mxr\_realloc\_caps(void \*ptr, size\_t newsize, uint32\_t caps);
void \*mxr\_zalloc\_caps(size\_t size, uint32\_t caps);

/\* query + diagnostics \*/
size\_t mxr\_get\_free\_size\_caps(uint32\_t caps);
size\_t mxr\_get\_min\_free\_size\_caps(uint32\_t caps);
void   mxr\_get\_status(mxr\_status\_t \*status);
bool   mxr\_get\_region\_status(int region\_index, mxr\_region\_status\_t \*status);
void   mxr\_dump(void);
```

### ESP heap compatibility (via `--wrap`)

```text
heap\_caps\_malloc(size, caps)        -> mxr\_malloc\_caps(size, caps)
heap\_caps\_free(ptr)                 -> mxr\_free(ptr)
heap\_caps\_calloc(n, size, caps)     -> mxr\_calloc\_caps(n, size, caps)
heap\_caps\_realloc(ptr, size, caps)  -> mxr\_realloc\_caps(ptr, size, caps)
heap\_caps\_zalloc(size, caps)        -> mxr\_zalloc\_caps(size, caps)
esp\_get\_free\_heap\_size()            -> mxr\_get\_free\_size\_caps(MALLOC\_CAP\_32BIT)
heap\_caps\_get\_dram\_free\_size()      -> mxr\_get\_free\_size\_caps(8BIT|32BIT|DMA)
```



## Usage Examples

### Basic usage

```c
#include "mxr\_malloc.h"

void app\_main(void)
{
    /\* ordinary allocation: DRAM first, IRAM fallback if needed \*/
    char \*buf = malloc(1024);
    if (buf) {
        memset(buf, 0, 1024);
        free(buf);
    }

    /\* DMA-safe buffer: always DRAM \*/
    void \*dma = heap\_caps\_malloc(512, MALLOC\_CAP\_DMA | MALLOC\_CAP\_32BIT);
    if (dma) heap\_caps\_free(dma);

    /\* executable memory: IRAM only \*/
    void \*code = heap\_caps\_malloc(256, MALLOC\_CAP\_EXEC | MALLOC\_CAP\_32BIT);
    if (code) heap\_caps\_free(code);
}
```

### Diagnostics

```c
mxr\_status\_t st;
mxr\_get\_status(\&st);
printf("free=%u min=%u desc=%u/%u exec=%u fallback=%u cross=%u invalid\_free=%u\\n",
       (unsigned)st.free\_bytes,
       (unsigned)st.min\_free\_bytes,
       (unsigned)st.active\_allocs,
       (unsigned)st.desc\_capacity,
       (unsigned)st.exec\_allocs,
       (unsigned)st.iram\_fallback\_allocs,
       (unsigned)st.cross\_region\_allocs,
       (unsigned)st.invalid\_free\_attempts);
```

### Full heap dump

```c
/\* Prints all regions and descriptors to UART \*/
mxr\_dump();
```



## Comparison with Original Heap

|Feature|Original ESP8266 heap|MxR-malloc|
|-|-|-|
|Metadata location|in-band (8 B/block)|out-of-band table (4 B/entry)|
|Free-space structure|linked free list per region|sorted descriptors + optional bitmap|
|Allocation search|O(N) list walk|O(N) gap scan or O(W) bitmap words|
|Max allocations|memory-bound|`CONFIG\_MXR\_MAX\_DESC` (hard cap)|
|Size-class regions|no|yes (configurable)|
|IRAM usage|IRAM tried first for 32BIT|DRAM first, IRAM as fallback|
|Cross-region fallback|no|yes (opt-in, last resort)|
|Heap tracing|`CONFIG\_HEAP\_TRACING`|not supported (disable it)|
|Per-block heap overhead|8 bytes|0 bytes in arena|

### Key behavioral differences

1. **DRAM-first policy** — the original heap tries IRAM first for `32BIT`;
MxR keeps IRAM as a fallback to reduce IRAM fragmentation.
2. **Descriptor limit** — MxR has a hard cap on simultaneous allocations.
Monitor `alloc\_fail\_table\_full` and raise `MXR\_MAX\_DESC` if needed.
3. **`realloc(ptr, 0)`** — frees the block and returns `NULL`
(configurable via `MXR\_REALLOC\_ZERO\_FREES`).



## Performance Considerations

All allocation operations run under `vPortETSIntrLock()` (interrupts
disabled). The hot path (`malloc`/`free`) is placed in **IRAM** by default;
`calloc`/`zalloc`/`realloc` join them only when
`CONFIG\_MXR\_IRAM\_PATH\_ALLOC\_FAMILY` is enabled.

Worst-case latency per operation:

```text
malloc:  O(N) free-block scan + O(N) descriptor shift
free:    O(log N) binary search + O(N) descriptor shift
```

where N = number of active descriptors.

|Scenario|Recommendation|
|-|-|
|<= 256 active allocs|default settings are fine|
|256-1024 active allocs|enable `MXR\_SEARCH\_BITMAP`|
|> 1024 active allocs|increase `MXR\_MAX\_DESC`, use bitmap|
|IRAM is tight|enable `MXR\_IRAM\_HOT\_PATH\_DISABLED`|
|calloc/realloc during flash ops|enable `MXR\_IRAM\_PATH\_ALLOC\_FAMILY`|



## Diagnostics

```c
typedef struct {
    bool     initialized;
    uint8\_t  region\_count;
    uint16\_t desc\_capacity;          /\* CONFIG\_MXR\_MAX\_DESC \*/
    uint16\_t active\_allocs;          /\* current descriptor count \*/
    uint16\_t max\_active\_allocs;      /\* peak descriptor count \*/
    size\_t   total\_bytes;            /\* DRAM + IRAM total \*/
    size\_t   free\_bytes;             /\* DRAM + IRAM free \*/
    size\_t   min\_free\_bytes;         /\* historical low watermark \*/
    size\_t   largest\_free\_block\_bytes;
    size\_t   iram\_total\_bytes;
    size\_t   iram\_free\_bytes;
    size\_t   iram\_min\_free\_bytes;
    uint32\_t exec\_allocs;            /\* EXEC allocations from IRAM \*/
    uint32\_t iram\_fallback\_allocs;   /\* 32BIT fallback allocations to IRAM \*/
    uint32\_t cross\_region\_allocs;    /\* cross-region DRAM fallback count \*/
    uint32\_t alloc\_fail\_no\_memory;
    uint32\_t alloc\_fail\_table\_full;
    uint32\_t invalid\_free\_attempts;  /\* double-free / wild pointer \*/
} mxr\_status\_t;
```

`mxr\_dump()` prints the full arena state: totals, per-region statistics, IRAM
state, fallback counters and every live descriptor.



## Project Structure

```text
mxr\_heap/
|-- CMakeLists.txt          # build config + linker --wrap flags
|-- Kconfig.projbuild       # menuconfig options
|-- include/
|   `-- mxr\_malloc.h        # public API + descriptor helpers
|-- mxr\_malloc.c            # core allocator
`-- mxr\_heap\_wrap.c         # linker --wrap integration (default mode)
```

Alternative integration files (`mxr\_heap\_compat.c`, `mxr\_heap\_port.c`) are
provided for replacement mode and must **never** be compiled together with the
wrap layer.



## FAQ

**Q: Why does `esp\_get\_free\_heap\_size()` differ from the original heap?**

The original heap reports DRAM + IRAM and tries IRAM first. With
`CONFIG\_MXR\_USE\_IRAM=y`, MxR also reports DRAM + IRAM for `32BIT` queries.
Both `free` and `minimum free` queries subtract the IRAM EXEC reserve for
non-EXEC requests, so the two numbers stay consistent. Compare
apples-to-apples with `heap\_caps\_get\_dram\_free\_size()` (DRAM only).

**Q: Can I use `CONFIG\_HEAP\_TRACING` with MxR?**

No. Disable it. A Kconfig warning is emitted if it is enabled.

**Q: What happens if the descriptor table fills up?**

`malloc` returns `NULL` and `alloc\_fail\_table\_full` is incremented, even if
memory is free. Increase `CONFIG\_MXR\_MAX\_DESC`.

**Q: Is MxR safe to call from ISRs?**

No. Like the original ESP8266 heap, do not call `malloc`/`free` from
interrupt handlers.

**Q: Why aren't `calloc`/`realloc` in IRAM by default?**

To keep the IRAM footprint small — only `malloc`/`free` are IRAM-resident by
default. If you allocate from contexts that run while flash cache is disabled
(e.g. flash-write callbacks), enable `CONFIG\_MXR\_IRAM\_PATH\_ALLOC\_FAMILY`, then
check IRAM usage with `idf.py size`. Note: like the original heap, the
allocator is still **not** ISR-safe.

**Q: Can I use MxR on ESP32?**

No. The memory map, linker symbols (`\_bss\_end`, `\_iram\_end`) and locking
primitives are ESP8266-specific.

**Q: How do I tune regions for my project?**

Run with defaults, call `mxr\_dump()`, and watch per-region utilization and
`iram\_fallback\_allocs`. Adjust `MXR\_REGION\_SIZES` / `MXR\_REGION\_PERCENTS`
until fallbacks are rare and no region is starved.

**Q: What is cross-region fallback?**

When enabled (`CONFIG\_MXR\_CROSS\_REGION\_FALLBACK=y`), if a block's own
size-class region and IRAM are both exhausted, the allocator tries other
DRAM regions. It picks the region with the smallest `min\_units` to minimize
fragmentation of large-block regions.

Disabled by default because it causes fragmentation. Monitor
`cross\_region\_allocs` — if it grows, increase the own region's percent in
`MXR\_REGION\_PERCENTS` or adjust `MXR\_REGION\_SIZES`.

**Q: What is `MXR\_REALLOC\_ZERO\_FREES`?**

Controls `realloc(ptr, 0)` behavior:

|Value|Behavior|
|-|-|
|`1` (default)|`free(ptr)`, return `NULL` (glibc / ESP-IDF style)|
|`0`|allocate a minimal 4-byte block|

Define it in your project before including `mxr\_malloc.h`:

```c
#define MXR\_REALLOC\_ZERO\_FREES 0
#include "mxr\_malloc.h"
```



## License

MIT License — see [LICENSE](LICENSE) for details.



<p align="center">
  <sub>Built for ESP8266 · Xtensa LX106 · FreeRTOS</sub>
</p>

