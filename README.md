<p align="center">
  <img src="https://img.shields.io/badge/platform-ESP8266-blue?logo=espressif" alt="Platform"/>
  <img src="https://img.shields.io/badge/SDK-ESP8266--RTOS--SDK-orange" alt="SDK"/>
  <img src="https://img.shields.io/badge/version-3.1-brightgreen" alt="Version"/>
  <img src="https://img.shields.io/badge/license-MIT-lightgrey" alt="License"/>
  <img src="https://img.shields.io/badge/IRAM--safe-important" alt="IRAM"/>
</p>

<h1 align="center">⚡ MxR-Malloc</h1>

<p align="center">
<b>Region-based capability-aware memory allocator for ESP8266</b><br/>
Drop-in replacement for the SDK heap with size-class regions, region-aware IRAM fallback, and zero-copy descriptors.
</p>

## 🎯 Why MxR-Malloc?

The stock ESP8266 heap is a single linked-list allocator over two flat regions (IRAM + DRAM). Every `malloc` walks the entire free-list, fragmentation is uncontrolled, and there is no way to reserve fast memory for small objects while keeping large contiguous blocks available for DMA / WiFi buffers.

MxR-Malloc splits the **DRAM** arena into configurable size-class regions, and the **IRAM** fallback zone into its own size-class regions. Small allocations never fragment the large-block region, and vice-versa.

| Feature | Stock SDK | MxR-Malloc |
| --- | --- | --- |
| Allocator | Linked-list first-fit | Descriptor gap-search (O(log n) lookup) |
| DRAM regions | 1 flat | 1–32 configurable size classes |
| IRAM heap | EXEC only | EXEC + region-aware 32-bit fallback |
| IRAM fallback regions | — | 1–32 configurable size classes |
| Free-block search | O(n) list walk | Sorted descriptor binary search |
| Fragmentation control | ❌ | ✅ per-region isolation |
| Cross-region fallback | — | ✅ directional last-resort (DRAM + IRAM fb) |
| IRAM-safe hot path | partial | ✅ malloc/free in IRAM |
| Heap tracing compat | ✅ | ⚠️ wrap mode only |
| Descriptor overhead | 8 B per block header | 8 B per descriptor (out-of-band) |

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        ESP8266 Memory Map                            │
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
│                                                                      │
│  0x40100000 ┌───────────────────────────────────────┐               │
│             │            IRAM  (~48 KB)             │               │
│             │  ┌──────────────┬───────────────────┐ │               │
│             │  │  EXEC zone   │     FB zone       │ │               │
│             │  │ [0, reserve) │ [reserve, end)    │ │               │
│             │  │  first-fit   │  fb size-classes  │ │               │
│             │  │  →           │  ← last-fit       │ │               │
│             │  └──────────────┴───────────────────┘ │               │
│             │            ↑ _iram_end                │               │
│  0x4010C000 └───────────────────────────────────────┘               │
│                                                                      │
│  Descriptors (out-of-band, 8 bytes each):                            │
│  ┌──────────────────────────────────────────────┐                    │
│  │ s_dram_desc[256]   │  s_iram_desc[128]       │                    │
│  │ sorted by offset   │  sorted by offset       │                    │
│  └──────────────────────────────────────────────┘                    │
└─────────────────────────────────────────────────────────────────────┘
```

EXEC blocks grow **from the start** of IRAM (first-fit). Fallback 32-bit blocks live in the fb zone `[reserve, iram_end)` and grow **from the end** (last-fit), so they stay away from the EXEC zone. The fb zone itself is split into size-class regions.

### Descriptor format (8 bytes)

```
off_flags (uint32_t):   [30..0] offset in bytes from arena base
len_flags (uint32_t):   [31]    EXEC flag
                        [30..0] length in bytes
```

No in-band headers → zero overhead per block, no coalescing needed, free is O(log n) binary search.

## 🚀 Quick Start

### 1. Add the component
```bash
cd components/
git clone https://github.com/YOUR_USER/mxr-malloc.git
```

### 2. Enable in `menuconfig`
```
idf.py menuconfig
  → Component config
    → MxR-Malloc
      → Integration mode: Wrap mode (default & safest)
      → Region boundaries:      "4-12%,128-14%,256-10%,512-25%,1280-0%"
      → IRAM fb region layout:  "4-0%"            (single flat fb region)
      → Enable IRAM heap: [*]
```

### 3. Build & flash
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
| `4-12%` | Region 0: blocks ≥ 4 bytes, gets 12% of the arena |
| `128-14%` | Region 1: blocks ≥ 128 bytes, gets 14% |
| `1280-0%` | Last region: unlimited, absorbs all leftover memory |

- The last region is always unlimited and absorbs leftover memory.
- Boundaries must be strictly increasing.
- Percent sum ≤ 100. Remainder goes to the last region.
- Empty IRAM-fb string → single flat fallback region (original behavior).
- Validated at CMake configure time (build fails on syntax error).

### Key options

| Option | Default | Description |
| --- | --- | --- |
| `MXR_MAX_DESC` | 256 | Max DRAM descriptors |
| `MXR_IRAM_MAX_DESC` | 128 | Max IRAM descriptors |
| `MXR_COMPACT_TYPES` | y | `uint16_t` for caps/min/max/count |
| `MXR_USE_IRAM` | y | Enable IRAM heap |
| `MXR_IRAM_RESERVE_BYTES` | 2048 | Reserve for EXEC allocs |
| `MXR_IRAM_FALLBACK_MAX_BYTES` | 0 (∞) | Max non-EXEC block in IRAM |
| `MXR_IRAM_FALLBACK_REGION_CONFIG` | `"4-0%"` | IRAM fb region layout |
| `MXR_CROSS_REGION_FALLBACK` | n | Last-resort cross-region alloc |
| `MXR_CROSS_REGION_CHECK_LARGEST` | n | Stats for fragmented cross-region skips |
| `MXR_IRAM_HOT_PATH_DISABLED` | n | Keep malloc/free out of IRAM |
| `MXR_IRAM_PATH_CORE` | default | Only malloc/free in IRAM |
| `MXR_IRAM_PATH_ALLOC_FAMILY` | — | + calloc/zalloc/realloc in IRAM |

## 🔌 Integration Modes

### Wrap mode (default, recommended)
Original `heap` component stays in the build. Linker `--wrap` redirects calls:
```
heap_caps_malloc()  →  __wrap__heap_caps_malloc()  →  mxr_malloc_caps()
```
Pros: zero risk, easy to disable. Cons: original allocator code still in flash (~1 KB).

### Compat mode
MxR replaces the original heap component. You must exclude `heap` from the build graph.

### Port mode
MxR provides standard libc `malloc`/`free`/`calloc`/`realloc` directly.

## 📦 API

### Standard heap API (drop-in)
```c
void *heap_caps_malloc(size_t size, uint32_t caps);
void  heap_caps_free(void *ptr);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void *heap_caps_zalloc(size_t size, uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_dram_free_size(void);
```

### MxR-native API
```c
void  mxr_init(void);
void *mxr_malloc_caps(size_t size, uint32_t caps);
void  mxr_free(void *ptr);
void *mxr_calloc_caps(size_t count, size_t size, uint32_t caps);
void *mxr_realloc_caps(void *ptr, size_t newsize, uint32_t caps);
void *mxr_zalloc_caps(size_t size, uint32_t caps);
void  mxr_get_status(mxr_status_t *status);
bool  mxr_get_region_status(int index, mxr_region_status_t *status);
bool  mxr_get_iram_fb_region_status(int index, mxr_region_status_t *status);
size_t mxr_get_free_size_caps(uint32_t caps);
size_t mxr_get_min_free_size_caps(uint32_t caps);
void  mxr_dump(void);
```

### Capability bits
```c
MALLOC_CAP_EXEC      (1 << 0)   // executable (IRAM only)
MALLOC_CAP_32BIT     (1 << 1)   // 32-bit aligned access
MALLOC_CAP_8BIT      (1 << 2)   // 8-bit access (DRAM only)
MALLOC_CAP_DMA       (1 << 3)   // DMA-capable (DRAM only)
MALLOC_CAP_INTERNAL  (1 << 11)  // internal memory
```

## 🧠 Allocation Strategy

```
malloc_caps(size, caps)
│
├─ caps & EXEC?
│   └─ IRAM only → first-fit from start → insert desc (EXEC flag)
│
├─ IRAM fallback allowed? (32BIT, no 8BIT/DMA/SPIRAM)
│   ├─ Step 1: own fb size-class region  (last-fit from end)
│   └─ Step 2: IRAM fb cross-region       (if enabled)
│   └─ found → insert desc → return (IRAM)
│
├─ DRAM — Step 1: own size-class region  (first-fit gap search)
│
├─ DRAM — Step 2: cross-region fallback   (if enabled)
│   └─ directional search away from the full region
│
└─ FAIL → return NULL
```

### Cross-region direction

When a region is full, candidates are tried **away from it**:

- region near the **start** → try larger regions first, then smaller
- region near the **end** → try smaller regions first, then larger

For 5 regions (`4,128,256,512,1280`):

| Block from | Search order |
| --- | --- |
| R0 (4B) | 1 → 2 → 3 → 4 |
| R1 (128B) | 2 → 3 → 4 → 0 |
| R2 (256B) | 3 → 4 → 1 → 0 |
| R3 (512B) | 2 → 1 → 0 → 4 |
| R4 (1280B) | 3 → 2 → 1 → 0 |

Free-block search and largest-free computation run in a **single pass** over the descriptor array.

### IRAM layout

EXEC blocks grow from the start, fallback blocks grow from the end of the fb zone, with a configurable reserve gap between them. This prevents fallback allocations from starving EXEC.

## 📊 Diagnostics

### Dump levels

| Level | Output |
| --- | --- |
| MINIMAL | total / free / min_free / largest (2 lines) |
| NORMAL | + regions, IRAM fb regions, counters (~20 lines) |
| FULL | + every descriptor (off/len/iram/exec) |

### Example output
```
I mxr_malloc: MxR dump: initialized=1
I mxr_malloc: total=86304 free=71200 min_free=68400 largest=65536
I mxr_malloc: desc dram=42/256 iram=3/128 max_active=45
I mxr_malloc: exec=3 iram_fb=2 cross=0 cross_skip=0
I mxr_malloc: DRAM: base=0x3ffe9a10 total=78832 free=71200 min_free=68400
I mxr_malloc: IRAM: base=0x4010a230 total=7472 free=5424 min_free=5424 fb_zone=5424
I mxr_malloc: iram_fb 0: start=2048 total=5424 min=4 max=0 free=5424 min_free=5424 largest=5424 alloc=2
I mxr_malloc: region 0: caps=0x0000080e start=0     total=9456  min=4    max=127  ...
I mxr_malloc: region 1: caps=0x0000080e start=9456  total=11032 min=128  max=255  ...
I mxr_malloc: region 2: caps=0x0000080e start=20488 total=7880  min=256  max=511  ...
I mxr_malloc: region 3: caps=0x0000080e start=28368 total=19708 min=512  max=1279 ...
I mxr_malloc: region 4: caps=0x0000080e start=48076 total=30756 min=1280 max=0    ...
I mxr_malloc: stats: fail_mem=0 fail_table=0 invalid_free=0
```

## ⚠️ Known Limitations

- **ESP8266 only** — arena limited to ~128 KB by 31-bit offset field.
- **No heap tracing** — `CONFIG_HEAP_TRACING` is incompatible with wrap mode.
- **IRAM byte access** — descriptor tables in IRAM require 32-bit-aligned access only (scalar state stays in DRAM).
- **No `realloc` in-place across regions** — block is moved if it doesn't fit.

## 📁 Project Structure
```
mxr-malloc/
├── CMakeLists.txt          # Build system, region validation, linker wraps
├── Kconfig.projbuild       # All configuration options
├── mxr_malloc.c            # Core allocator
├── include/
│   └── mxr_malloc.h        # Public API and data structures
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
