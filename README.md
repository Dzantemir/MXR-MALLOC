<p align="center">
  <img src="https://img.shields.io/badge/platform-ESP8266-blue?logo=espressif" alt="Platform"/>
  <img src="https://img.shields.io/badge/SDK-ESP8266--RTOS--SDK-orange" alt="SDK"/>
  <img src="https://img.shields.io/badge/version-3.2-brightgreen" alt="Version"/>
  <img src="https://img.shields.io/badge/license-MIT-lightgrey" alt="License"/>
  <img src="https://img.shields.io/badge/IRAM--safe-important" alt="IRAM"/>
</p>

<h1 align="center">⚡ MxR-Malloc</h1>

<p align="center">
  <b>Region-based capability-aware memory allocator for ESP8266</b><br/>
  Drop-in replacement for the SDK heap with size-class regions, a hard-bound
  IRAM EXEC zone, region-aware IRAM fallback, anti-sliver expansion and
  zero-copy descriptors.
</p>

## 🎯 Why MxR-Malloc?

The stock ESP8266 heap is a single linked-list allocator over two flat regions
(IRAM + DRAM). Every `malloc` walks the entire free-list, fragmentation is
uncontrolled, and there is no way to reserve fast memory for small objects
while keeping large contiguous blocks available for DMA / WiFi buffers.

MxR-Malloc splits the DRAM arena into configurable size-class regions and the
IRAM fallback zone into its own size-class regions. Small allocations never
fragment the large-block region, and vice-versa. EXEC allocations get a
dedicated, hard-reserved zone at the start of IRAM.

| Feature | Stock SDK | MxR-Malloc |
| --- | --- | --- |
| Allocator | Linked-list first-fit | Descriptor gap-search (O(log n) lookup) |
| DRAM regions | 1 flat | 1–32 configurable size classes |
| IRAM heap | EXEC only | EXEC zone + region-aware 32-bit fallback |
| IRAM fallback regions | — | 1–32 configurable size classes |
| EXEC placement | Anywhere in IRAM | Hard-bound to `[0, reserve)` |
| Free-block search | O(n) list walk | Sorted descriptor binary search |
| Best-fit early exit | — | ✅ waste-threshold early exit |
| Anti-sliver expansion | — | ✅ absorbs tiny leftover gaps |
| Fragmentation control | ❌ | ✅ per-region isolation |
| Cross-region fallback | — | ✅ directional, per-arena guards |
| IRAM-safe hot path | partial | ✅ malloc/free in IRAM |
| Heap tracing compat | ✅ | ⚠️ wrap mode only |
| Descriptor overhead | 8 B per block header | 8 B per descriptor (out-of-band) |

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        ESP8266 Memory Map                           │
├─────────────────────────────────────────────────────────────────────┤
│  0x3FFE8000 ┌───────────────────────────────────────┐               │
│             │            DRAM  (~80 KB)             │               │
│             │  ┌──────┬──────┬──────┬──────┬──────┐ │               │
│             │  │  R0  │  R1  │  R2  │  R3  │  R4  │ │ ← size classes│
│             │  │ ≥4B  │≥128B │≥256B │≥512B │≥1280B│ │   from Kconfig│
│             │  │ 12%  │ 14%  │ 10%  │ 25%  │ rest │ │               │
│             │  └──────┴──────┴──────┴──────┴──────┘ │               │
│             │            ↑ _bss_end                 │               │
│  0x40000000 └───────────────────────────────────────┘               │
│                                                                     │
│  0x40100000 ┌───────────────────────────────────────┐               │
│             │            IRAM  (~48 KB)             │               │
│             │  ┌──────────────┬───────────────────┐ │               │
│             │  │  EXEC zone   │     FB zone       │ │               │
│             │  │ [0, reserve) │ [reserve, end)    │ │               │
│             │  │  first-fit → │ ← last-fit        │ │               │
│             │  │  HARD-bound  │  fb size-classes  │ │               │
│             │  └──────────────┴───────────────────┘ │               │
│             │            ↑ _iram_end                │               │
│  0x4010C000 └───────────────────────────────────────┘               │
│                                                                     │
│  Descriptors (out-of-band, 8 bytes each):                           │
│  ┌──────────────────────────────────────────────┐                   │
│  │ s_dram_desc[256]   │  s_iram_desc[128]       │                   │
│  │ sorted by offset   │  sorted by offset       │                   │
│  └──────────────────────────────────────────────┘                   │
└─────────────────────────────────────────────────────────────────────┘
```

**EXEC zone** — `[0, CONFIG_MXR_IRAM_RESERVE_BYTES)`. EXEC blocks are placed
**only** inside this zone (hard binding, both on `malloc` and `realloc` grow)
using first-fit from the start. Setting the reserve to `0` disables EXEC
allocations entirely (every `MALLOC_CAP_EXEC` request returns `NULL` and
increments `exec_zone_rejects`).

**Fallback zone** — `[reserve, iram_end)`. Pure 32-bit non-EXEC allocations
that fit the fallback criteria live here, split into size-class regions, and
grow from the end (last-fit) so they stay away from the EXEC zone.

**Descriptor format (8 bytes)** — no in-band headers, zero per-block overhead,
no coalescing needed, free is an O(log n) binary search:

```
off_flags (uint32_t):   [30..0] offset in bytes from arena base
len_flags (uint32_t):   [31]    EXEC flag
                        [30..0] length in bytes
```

## 🧠 Allocation Strategy

```
malloc_caps(size, caps)
 │
 ├─ caps & EXEC ?
 │   └─ IRAM only (hard-bound EXEC zone)
 │       ├─ exec_zone_end == 0 ? → REJECT (exec_zone_rejects++)
 │       ├─ first-fit in [0, reserve)
 │       ├─ size > zone ? → REJECT (exec_zone_rejects++)
 │       └─ no gap      ? → REJECT (alloc_fail_no_memory++)
 │       └─ found → insert desc (EXEC flag) → return (IRAM)
 │
 ├─ IRAM fallback allowed ? (32BIT, no 8BIT/DMA/SPIRAM)
 │   ├─ Step 1: own fb size-class region  (last-fit from end)
 │   └─ Step 2: IRAM fb cross-region      (if MXR_IRAM_CROSS_ENABLED)
 │       ├─ max_bytes GUARD   (skipped when preset = All)
 │       ├─ min_bytes guard   (skipped when preset = All)
 │       └─ directional search
 │   └─ found → insert desc → return (IRAM)
 │
 ├─ DRAM — Step 1: own size-class region
 │   └─ best-fit with early exit + anti-sliver expansion
 │
 ├─ DRAM — Step 2: cross-region fallback (if MXR_DRAM_CROSS_ENABLED)
 │   ├─ max_bytes GUARD   (skipped when preset = All)
 │   ├─ min_bytes guard   (skipped when preset = All)
 │   └─ directional search
 │
 └─ FAIL → return NULL
```

### Anti-sliver expansion

When a block is cut from a gap and the leftover tail would be smaller than
`MXR_MIN_SLICE_BYTES`, the block is expanded to consume the entire gap,
preventing unusable micro-fragments. Expansion is capped by the region's
`max_bytes` so a block never crosses its size class. The same rule protects
`realloc` shrink (a tiny tail is not split off) and `realloc` grow (a tiny
trailing gap is absorbed).

### Best-fit early exit

The free-gap search stops as soon as a gap with
`waste <= size >> MXR_BEST_FIT_WASTE_SHIFT` is found. Disabling it forces a
strict best-fit full scan (only an exact-fit gap stops early).

## 🔀 Cross-region fallback

Cross-region fallback is a **last resort** used when a block cannot fit in its
own size-class region. It is controlled by a master switch plus **independent**
per-arena enable switches and guards:

- `MXR_CROSS_REGION_FALLBACK` — master switch.
- `MXR_DRAM_CROSS_ENABLED` — DRAM cross-region on/off.
- `MXR_IRAM_CROSS_ENABLED` — IRAM fallback cross-region on/off.

Each cross-region placement is subject to two guards (DRAM and IRAM each have
their own pair):

| Guard | Purpose | Presets |
| --- | --- | --- |
| **max_bytes GUARD** | Protects a target region from blocks too large for its size class. Rejects `bytes > max_bytes × N/D`. | Conservative 50% · Moderate 75% · Aggressive 90% · **All** (no check) |
| **min_bytes guard** | Protects large-block regions from tiny allocations. Skips when `bytes × DIVISOR < min_bytes`. | Conservative ÷1 · Moderate ÷2 · Aggressive ÷4 · **All** (no check) |

Choosing **All** disables that guard entirely. Rejections are counted in
`cross_region_guard_rejects`. When a region is scanned but no gap is found,
`cross_region_skip_fragmented` is incremented.

The search is **directional**: regions are tried moving away from the full
region (larger regions first when the full region is in the lower half, smaller
regions first otherwise).

## 🚀 Quick Start

**1. Add the component**

```bash
cd components/
git clone https://github.com/YOUR_USER/mxr-malloc.git
```

**2. Enable in `menuconfig`**

```
idf.py menuconfig
  → Component config
    → MxR-Malloc
      → Integration mode: Wrap mode (default & safest)
      → Region boundaries:      "4-12%,128-14%,256-10%,512-25%,1280-0%"
      → IRAM fb region layout:  "4-0%"            (single flat fb region)
      → Enable IRAM heap: [*]
      → IRAM_RESERVE_BYTES: 2048
```

**3. Build & flash**

```bash
idf.py build flash monitor
```

At boot you will see:

```
I (123) mxr_malloc: init ok: base=0x3ffe9a10 bytes=78832 dram_desc=256 iram_desc=128
I (126) mxr_malloc: IRAM heap ok: base=0x4010a230 bytes=7472 fb_zone=5424 fb_regions=1
```

## ⚙️ Configuration

### Region layout

Both DRAM and IRAM-fallback topologies are defined by one string each:

```
CONFIG_MXR_REGION_CONFIG="4-12%,128-14%,256-10%,512-25%,1280-0%"
CONFIG_MXR_IRAM_FALLBACK_REGION_CONFIG="4-0%"
```

| Entry | Meaning |
| --- | --- |
| `4-12%` | Region 0: blocks ≥ 4 bytes, gets 12% of the zone |
| `128-14%` | Region 1: blocks ≥ 128 bytes, gets 14% |
| `1280-0%` | Last region: unlimited, absorbs all leftover memory |

- Rules: the last region is always unlimited and absorbs leftover memory;
- boundaries must be strictly increasing; percent sum ≤ 100; an empty IRAM-fb
- string yields a single flat fallback region. Validated at CMake configure time.

### Key options

| Option | Default | Description |
| --- | --- | --- |
| `MXR_MAX_DESC` | 256 | Max DRAM descriptors |
| `MXR_IRAM_MAX_DESC` | 128 | Max IRAM descriptors |
| `MXR_COMPACT_TYPES` | y | `uint16_t` for caps/min/max/count |
| `MXR_USE_IRAM` | y | Enable IRAM heap |
| `MXR_IRAM_RESERVE_BYTES` | 2048 | Hard-bound EXEC zone `[0, reserve)`; `0` disables EXEC |
| `MXR_IRAM_FALLBACK_MAX_BYTES` | 0 (∞) | Max non-EXEC block allowed in IRAM |
| `MXR_IRAM_FALLBACK_REGION_CONFIG` | `"4-0%"` | IRAM fb region layout |
| `MXR_CROSS_REGION_FALLBACK` | y | Cross-region master switch |
| `MXR_DRAM_CROSS_ENABLED` | y | DRAM cross-region on/off |
| `MXR_IRAM_CROSS_ENABLED` | y | IRAM fb cross-region on/off |
| `MXR_ANTI_SLIVER` | y | Absorb tiny leftover gaps |
| `MXR_MIN_SLICE_BYTES` | 8 | Anti-sliver threshold (4–64) |
| `MXR_BEST_FIT_EARLY_EXIT` | y | Best-fit early exit |
| `MXR_BEST_FIT_WASTE_SHIFT` | 2 | Early-exit waste threshold (1–4) |
| `MXR_IRAM_HOT_PATH_DISABLED` | n | Keep malloc/free out of IRAM |

### Descriptor table placement

| Option | Description |
| --- | --- |
| `MXR_DESC_IN_DRAM` | DRAM `.bss` — default, safest |
| `MXR_DESC_IN_IRAM_TEXT` | IRAM `.iram0.text` — no linker patch needed |
| `MXR_DESC_IN_IRAM_BSS` | IRAM `.iram0.bss` — requires patched linker script |

> Only the descriptor arrays can move to IRAM. Scalar state stays in DRAM
> because ESP8266 IRAM does not support byte/half-word accesses safely.

## 🔌 Integration Modes

**Wrap mode (default, recommended)** — original `heap` component stays in the
build; linker `--wrap` redirects calls. Pros: zero risk, easy to disable.

```
heap_caps_malloc()  →  __wrap__heap_caps_malloc()  →  mxr_malloc_caps()
```

**Compat mode** — MxR replaces the original heap component. You must exclude
`heap` from the build graph.

**Port mode** — MxR provides standard libc `malloc`/`free`/`calloc`/`realloc`
directly.

Optional wraps (Wrap mode): `MXR_WRAP_HEAP_QUERY`, `MXR_WRAP_DEFAULT_POOL`,
`MXR_WRAP_ESP_SYSTEM`, `MXR_WRAP_LIBC`, `MXR_WARN_HEAP_TRACING`.

## 📦 API

### Standard heap API (drop-in)

```c
void  *heap_caps_malloc(size_t size, uint32_t caps);
void   heap_caps_free(void *ptr);
void  *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void  *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void  *heap_caps_zalloc(size_t size, uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_dram_free_size(void);
```

### MxR-native API

```c
void   mxr_init(void);
void  *mxr_malloc_caps(size_t size, uint32_t caps);
void   mxr_free(void *ptr);
void  *mxr_calloc_caps(size_t count, size_t size, uint32_t caps);
void  *mxr_realloc_caps(void *ptr, size_t newsize, uint32_t caps);
void  *mxr_zalloc_caps(size_t size, uint32_t caps);
void   mxr_get_status(mxr_status_t *status);
bool   mxr_get_region_status(int index, mxr_region_status_t *status);
bool   mxr_get_iram_fb_region_status(int index, mxr_region_status_t *status);
size_t mxr_get_free_size_caps(uint32_t caps);
size_t mxr_get_min_free_size_caps(uint32_t caps);
void   mxr_dump(void);
```

### Capability bits

```c
MALLOC_CAP_EXEC      (1 << 0)   // executable (IRAM EXEC zone only)
MALLOC_CAP_32BIT     (1 << 1)   // 32-bit aligned access
MALLOC_CAP_8BIT      (1 << 2)   // 8-bit access (DRAM only)
MALLOC_CAP_DMA       (1 << 3)   // DMA-capable (DRAM only)
MALLOC_CAP_SPIRAM    (1 << 10)  // compatibility
MALLOC_CAP_INTERNAL  (1 << 11)  // internal memory
```

## 📊 Diagnostics

### Dump levels

| Level | Output |
| --- | --- |
| `MXR_DUMP_MINIMAL` | total / free / min_free / largest (2 lines) |
| `MXR_DUMP_NORMAL` | + regions, IRAM fb regions, EXEC zone, counters (~25 lines) |
| `MXR_DUMP_FULL` | + every descriptor (off/len/iram/exec) |

### Example output

```
I mxr_malloc: MxR dump: initialized=1
I mxr_malloc: total=86304 free=71200 min_free=68400 largest=65536
I mxr_malloc: desc dram=42/256 iram=3/128 max_active=45
I mxr_malloc: exec=3 iram_fb=2 cross=0 cross_skip=0 guard_rej=0
I mxr_malloc: frag: pct=12% gaps=6 slivers=2(33%) bf_early=31 anti_sliver=2
I mxr_malloc: DRAM: base=0x3ffe9a10 total=78832 free=71200 min_free=68400
I mxr_malloc: IRAM: base=0x4010a230 total=7472 free=5424 min_free=5424 fb_zone=5424 exec_zone=2048 exec_free=1920 exec_rejects=0
I mxr_malloc: iram_fb 0: start=2048 total=5424 min=4 max=0 free=5424 min_free=5424 largest=5424 alloc=2
I mxr_malloc: region 0: caps=0x0000080e start=0     total=9456  min=4    max=127  ...
I mxr_malloc: region 1: caps=0x0000080e start=9456  total=11032 min=128  max=255  ...
I mxr_malloc: region 2: caps=0x0000080e start=20488 total=7880  min=256  max=511  ...
I mxr_malloc: region 3: caps=0x0000080e start=28368 total=19708 min=512  max=1279 ...
I mxr_malloc: region 4: caps=0x0000080e start=48076 total=30756 min=1280 max=0    ...
I mxr_malloc: stats: fail_mem=0 fail_table=0 invalid_free=0
```

### Notable counters (`mxr_status_t`)

| Field | Meaning |
| --- | --- |
| `exec_allocs` | EXEC allocations served from the EXEC zone |
| `exec_zone_rejects` | EXEC requests rejected (zone empty / block too large) |
| `iram_fallback_allocs` | Non-EXEC 32-bit allocations placed in IRAM |
| `cross_region_allocs` | Cross-region placements |
| `cross_region_guard_rejects` | Cross-region attempts rejected by GUARD / min_bytes guard |
| `cross_region_skip_fragmented` | Cross-region scans that found no gap |
| `fragmentation_pct` | `(free − largest) / free × 100` |
| `gap_count` / `sliver_count` | Free gaps / gaps below `MXR_MIN_SLICE_BYTES` |
| `best_fit_early_exits` | Best-fit searches that stopped early |
| `anti_sliver_expansions` | Blocks expanded to absorb a sliver |
| `iram_exec_zone_total/free/min_free` | EXEC zone capacity / free / low-water mark |

## ⚠️ Known Limitations

- **ESP8266 only** — arena limited to ~128 KB by the 31-bit offset field.
- **No heap tracing** — `CONFIG_HEAP_TRACING` is incompatible with wrap mode.
- **IRAM byte access** — descriptor tables in IRAM require 32-bit-aligned
  access only (scalar state stays in DRAM).
- **No in-place `realloc` across regions** — the block is moved if it does not
  fit.
- **EXEC is DRAM-invisible** — `MALLOC_CAP_EXEC` never falls back to DRAM and
  never leaves the EXEC zone.

## 📁 Project Structure

```
mxr-malloc/
├── CMakeLists.txt          # Build system, region validation, linker wraps
├── Kconfig.projbuild       # All configuration options
├── mxr_malloc.c            # Core allocator
├── mxr_malloc.h            # Public API and data structures
├── mxr_heap_wrap.c         # Linker --wrap integration layer
├── mxr_heap_compat.c       # Direct heap_caps_* replacement
└── mxr_heap_port.c         # Standard libc replacement
```

## 🧪 Testing

```c
#include "mxr_malloc.h"

void app_main(void)
{
    mxr_init();
    mxr_dump();

    void *ptrs[128];
    for (int i = 0; i < 128; i++) {
        ptrs[i] = mxr_malloc(16 + i * 8);
    }
    for (int i = 0; i < 128; i++) {
        mxr_free(ptrs[i]);
    }

    // EXEC allocation — hard-bound to [0, reserve)
    void *exec = mxr_malloc_caps(256, MALLOC_CAP_EXEC);

    mxr_dump();
}
```

## 📜 License

MIT — see [LICENSE](LICENSE) for details.

## 🙏 Acknowledgments

- ESP8266 RTOS SDK heap implementation for reference
- ESP-IDF `heap_caps` API design
- FreeRTOS community

<p align="center">
  <sub>Made with ❤️ for ESP8266</sub>
</p>
