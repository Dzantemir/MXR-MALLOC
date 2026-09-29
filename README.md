> **2026-09-29 audit fixes:** see [docs/CHANGES-RU.md](docs/CHANGES-RU.md)
> and [docs/VALIDATION.md](docs/VALIDATION.md) for changes, reproducible tests,
> target-build results and the remaining on-device validation requirements.

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="logo-full.svg"/>
    <source media="(prefers-color-scheme: light)" srcset="logo-full-light.svg"/>
    <img src="logo-full-light.svg" width="480" alt="MxR-Malloc"/>
  </picture>
</p>

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
  IRAM EXEC zone, region-aware IRAM fallback, anti-sliver expansion,
  zero-copy descriptors and a Kconfig-selectable, load-store-safe IRAM layout.
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
| **IRAM byte/half-word access** | silently emulated by an exception handler (see [IRAM safety](#-iram-safety)) | ✅ **impossible by construction**: state is either in DRAM or all-32-bit |
| Placement audit | ❌ | ✅ `tools/check_iram_widths.py` (CI-friendly) |
| Per-allocation placement hint | ❌ | ✅ `MXR_CAP_PREFER_IRAM` (soft "IRAM first") |
| Query cost of free space | O(n) walk | O(1) quick-fit hint, optional binning |
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
│             │  │  first-fit → │ best-fit, block   │ │               │
│             │  │  HARD-bound  │ top-aligned in gap│ │               │
│             │  └──────────────┴───────────────────┘ │               │
│             │            ↑ _iram_end                │               │
│  0x4010C000 └───────────────────────────────────────┘               │
│                                                                     │
│  Descriptors (out-of-band, 8 bytes each):                           │
│  ┌──────────────────────────────────────────────┐                   │
│  │ s_dram_desc[256]   │  s_iram_desc[128]       │                   │
│  │ sorted by offset   │  sorted by offset       │                   │
│  └──────────────────────────────────────────────┘                   │
│                                                                     │
│  Static state (not part of the arenas):                             │
│  ┌──────────────────────────────────────────────┐                   │
│  │ s_stats  (mxr_status_t)                      │                   │
│  │ s_region[N] / s_iram_fb_region[M]            │                   │
│  │ → DRAM (.bss) by default, or IRAM with       │                   │
│  │   all-32-bit fields (see IRAM safety)        │                   │
│  └──────────────────────────────────────────────┘                   │
└─────────────────────────────────────────────────────────────────────┘
```

**EXEC zone** — `[0, CONFIG_MXR_IRAM_RESERVE_BYTES)`. EXEC blocks are placed
**only** inside this zone (hard binding, both on `malloc` and `realloc` grow)
using first-fit from the start. Setting the reserve to `0` disables EXEC
allocations entirely (every `MALLOC_CAP_EXEC` request returns `NULL` and
increments `exec_zone_rejects`).

**Fallback zone** — `[reserve, iram_end)`. Non-EXEC allocations that carry
`MALLOC_CAP_32BIT`, `MALLOC_CAP_INTERNAL`, or no caps at all (and do NOT
carry `8BIT`/`DMA`/`SPIRAM`) live here, split into size-class regions.
Inside a region the search is **best-fit with early-exit**; among equal gaps
the higher address is preferred, and the block is placed at the **top** of
the chosen gap so that the low-address side stays free for EXEC-adjacent
usage. `MXR_CAP_PREFER_IRAM` lets a caller ask for IRAM first regardless of
the global order.

**Descriptor format (8 bytes)** — no in-band headers, zero per-block overhead,
no coalescing needed, free is an O(log n) binary search:

```
off_flags (uint32_t):   [30..0] offset in bytes from arena base
len_flags (uint32_t):   [31]    EXEC flag
                        [30..0] length in bytes
```

## 🧱 IRAM safety

### The rule

ESP8266 (LX106) can access IRAM (`0x40100000..`) **only with 32-bit
loads/stores**. Any 8- or 16-bit access, and any unaligned 32-bit access, is
not executed by the core — it raises `EXCCAUSE_LOAD_STORE_ERROR`.

### Why violations do not crash immediately

The SDK hides this. `startup.c` installs exception vectors before user code
runs, with an explicit comment about non-4-byte accesses:

```c
/* exception vect must be initialized. And then user can load/store
   data which is not aligned by 4-byte */
__asm__ __volatile__("movi a0, 0x40100000\n wsr a0, vecbase");
```

`LoadStoreErrorHandler` (`components/freertos/port/esp8266/xtensa_vectors.S`)
then emulates exactly five opcodes — `l8ui`, `l16si`, `l16ui` (loads) and
`s8i`, `s16i` (stores, implemented as read-modify-write of the containing
word). Everything else goes to `_xt_ext_panic()`.

So byte/half-word accesses to IRAM **work** — at the cost of a full exception
per access: register save, opcode decode at `EPC1`, emulation, `EPC1 += 3`,
`SAR` restore, `rfe`. That happens on a fixed 28-byte handler stack with a
single re-entry slot, and in MxR-Malloc it happens **inside `mxr_lock()`**,
i.e. with interrupts disabled. This is latency, not corruption — which is why
the mistake is easy to ship.

> Historical note: an earlier revision placed `s_stats`, `s_region[]` and
> `s_iram_fb_region[]` into IRAM while they still had `bool` / `uint8_t` /
> `uint16_t` fields. A source-level audit found **132 sub-32-bit accesses to
> IRAM objects, 17 of them in the `malloc`/`free`/`realloc` hot path**
> (`mxr_region_for_size`, `mxr_region_size_ok`, `mxr_region_caps_ok`,
> `mxr_region_invalidate_cache`, `mxr_free_locked`, …). Nothing crashed —
> every one of those accesses was being emulated by the handler.

### What the code enforces now

State objects (`s_stats`, `s_region[]`, `s_iram_fb_region[]`) are declared with
`MXR_STATE_DATA_ATTR`, whose meaning is chosen in `menuconfig`:

| | **Variant A — `MXR_STATE_IN_DRAM`** | **Variant B — `MXR_STATE_IN_IRAM`** (default when tables are in IRAM) |
| --- | --- | --- |
| Attribute | empty → objects land in `.bss` | `MXR_IRAM_DATA_ATTR` → objects stay in IRAM |
| Field widths | natural (`bool`/`uint8_t`/`uint16_t`, `COMPACT_TYPES` applies) | forced to 32 bit via `MXR_FIELD_BOOL` / `_U8` / `_U16`; `COMPACT_TYPES` stops applying to `mxr_region_t` / `mxr_status_t` |
| Size-class typedefs | `uint16_t` under `COMPACT_TYPES` | forced `uint32_t` |
| Static state bytes¹ | `232 + 44 × (N + M)` in **DRAM** | `256 + 48 × (N + M)` in **IRAM** |
| Regression barrier | external audit only | `_Static_assert` barrier **fails the build** if any placed field is not 32 bit |
| Pick it when | DRAM is available and you want the smallest diff | DRAM is scarce (lwIP profiles) and IRAM can absorb ~50–70 B |

¹ `N` = DRAM size-class regions, `M` = IRAM fallback regions; measured with
`MXR_QUICK_FIT_HINT_ENABLE=y`, `MXR_BINNING=n`. Enabling `MXR_BINNING` widens
`mxr_region_t` by 24 B per region (bin arrays), disabling the quick-fit hint
shrinks it by 8 B. Example: 4 DRAM regions + 2 fallback regions → variant A
takes **496 B of DRAM**, variant B takes **544 B of IRAM** (Δ = 48 B).

Two guard rails in `include/mxr_malloc.h`:

```c
/* state in IRAM is only possible together with IRAM descriptor tables */
#if defined(CONFIG_MXR_STATE_IN_IRAM) && !MXR_IRAM_PLACEMENT_ACTIVE
#error "CONFIG_MXR_STATE_IN_IRAM requires IRAM descriptor placement ..."
#endif

/* in variant B every field of a placed type must be one word wide */
MXR_ASSERT_WORD_FIELD(mxr_region_t, max_bytes);
MXR_ASSERT_WORD_FIELD(mxr_status_t, max_active_allocs);
/* ... */
```

Values that bypass `COMPACT_TYPES` on purpose: the quick-fit hint fields
(`hint_len`, `hint_len[MXR_BIN_COUNT]`) are always `uint32_t` — they cache
byte lengths and are not part of the size-class packing.

### Audit your tree

`tools/check_iram_widths.py` is a dependency-free, source-level audit of the
rule. It resolves `MXR_STATE_DATA_ATTR` / `MXR_IRAM_DATA_ATTR` objects, computes
field widths **for both placement variants**, follows aliases such as
`mxr_region_t *r = &s_region[0]`, and classifies each access as hot or cold
path:

```bash
python3 tools/check_iram_widths.py mxr_malloc/        # exit 1 if it finds anything
```

```
РЕЖИМ B (MXR_STATE_PLACEMENT = IRAM, поля 32 бита)
  s_stats                : mxr_status_t   -> безопасно (все поля 32-бит)
  s_region               : mxr_region_t   -> безопасно (все поля 32-бит)
  Нарушений не найдено.
РЕЖИМ A (MXR_STATE_PLACEMENT = DRAM, состояние в .bss)
  Нарушений не найдено.
ИТОГ: нарушений нет ни в одном проверенном режиме.
```

Wire it into the build so the rule cannot silently regress:

```cmake
execute_process(COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_iram_widths.py
                        ${CMAKE_CURRENT_SOURCE_DIR}
                RESULT_VARIABLE mxr_audit)
if(NOT mxr_audit EQUAL 0)
    message(FATAL_ERROR "MxR IRAM width audit failed (see output above)")
endif()
```

### See the cost on hardware (5 minutes)

```c
static uint32_t v_word __attribute__((section(".iram0.text"), aligned(4)));
static uint16_t v_half __attribute__((section(".iram0.text"), aligned(4)));

void bench_iram_width(void)
{
    volatile int i;
    uint32_t t0 = system_get_time();
    for (i = 0; i < 100000; i++) v_word++;      /* legal 32-bit access   */
    uint32_t word_us = system_get_time() - t0;

    t0 = system_get_time();
    for (i = 0; i < 100000; i++) v_half++;      /* emulated by the handler */
    uint32_t half_us = system_get_time() - t0;

    os_printf("IRAM word++: %u us | IRAM half++: %u us | ratio x%.1f\n",
              word_us, half_us, (double)half_us / (double)(word_us ? word_us : 1));
}
```

Expect the half-word loop to be several times slower — each iteration traps
twice (one `l16ui` load, one `s16i` store). That is the price an unsuspecting
8/16-bit IRAM field pays on every single access.

## 🧠 Allocation Strategy

```
malloc_caps(size, caps)
 │
 ├─ MXR_CAP_PREFER_IRAM set ?
 │   └─ strip the bit, remember the preference (it never causes a failure)
 │
 ├─ caps & EXEC ?
 │   └─ IRAM only (hard-bound EXEC zone)
 │       ├─ exec_zone_end == 0 ? → REJECT (exec_zone_rejects++)
 │       ├─ first-fit in [0, reserve)
 │       ├─ size > zone ? → REJECT (exec_zone_rejects++)
 │       └─ no gap      ? → REJECT (alloc_fail_no_memory++)
 │       └─ found → insert desc (EXEC flag) → return (IRAM)
 │
 ├─ [MXR_CAP_PREFER_IRAM] → IRAM fallback attempt (before DRAM)
 │
 ├─ [CONFIG_MXR_IRAM_FB_ORDER_IRAM_FIRST]
 │   └─ IRAM fallback attempt (BEFORE DRAM — original MxR order)
 │
 ├─ DRAM — Step 1: own size-class region
 │   └─ best-fit with early exit + anti-sliver expansion
 │
 ├─ DRAM — Step 2: cross-region fallback (if MXR_DRAM_CROSS_ENABLED)
 │   ├─ max_bytes GUARD   (skipped when preset = All)
 │   ├─ min_bytes guard   (skipped when preset = All)
 │   └─ directional search
 │
 ├─ [CONFIG_MXR_IRAM_FB_ORDER_DRAM_FIRST]  ← DEFAULT
 │   └─ IRAM fallback attempt (AFTER DRAM fails)
 │
 └─ FAIL → return NULL

 IRAM fallback attempt (shared by both orders):
  │  Allowed when: caps has 32BIT or INTERNAL or caps==0,
  │                and caps has NO 8BIT / DMA / SPIRAM / EXEC,
  │                and IRAM fb zone is non-empty, and block ≤ FB_MAX
  │
  ├─ Step 1: own fb size-class region
  │   └─ best-fit + early exit, block placed at TOP of gap
  └─ Step 2: IRAM fb cross-region (if MXR_IRAM_CROSS_ENABLED)
      ├─ max_bytes GUARD   (skipped when preset = All)
      ├─ min_bytes guard   (skipped when preset = All)
      └─ directional search, block placed at TOP of gap
  └─ found → insert desc → return (IRAM)
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

## ⚡ Optimization & debug features (the `Optimization` menu)

| Option | Default | What it does |
| --- | --- | --- |
| `MXR_QUICK_FIT_HINT_ENABLE` | **y** | Per-region O(1) hint: caches the largest known gap, so a repeated size query skips the search. Hint fields are always 32-bit. |
| `MXR_BINNING` | n | Segregated per-bin LRU gap lists inside each region (larger gaps first, near-fit bins first). Requires the quick-fit hint. Adds 24 B per region. |
| `MXR_BIN_COUNT` | 4 | Number of bins per region (2–8). |
| `MXR_BIG_GAP_GUARD` | n | Prevents premature carving of large gaps (a “baton” policy): a big gap is only split when the request is big enough. |
| `MXR_BIG_GAP_MIN` | 1024 | Only gaps ≥ this size are guarded. |
| `MXR_BIG_GAP_FACTOR_SHIFT` | 2 | Guard ratio: block must be ≥ `gap >> shift`. |
| `MXR_DESC_DYNAMIC` | n | Descriptor table carved from the DRAM heap tail, grows/shrinks in chunks (needs tables in DRAM). |
| `MXR_DESC_INIT` / `MXR_DESC_CHUNK` | 32 / 16 | Initial table size and growth chunk (in descriptors). |
| `MXR_IRAM_DESC_DYNAMIC` | n | Same idea for the IRAM fallback heap (requires the fallback zone). |
| `MXR_CANARY` | n | 4-byte head + 4-byte tail canary per block (**+8 B per block**). Corrupt detection quarantines the block on `free`/`realloc`. Debug only — see Known Limitations. |
| `MXR_DOUBLE_FREE_DETECT` | n | Ring of recent frees; a repeated pointer is reported instead of corrupting the heap. |
| `MXR_DFD_RING_SIZE` | 16 | Ring size (4–256). |

All of these report through `mxr_status_t` counters (`quick_fit_hint_hits/misses`,
`canary_violations`, `double_free_detects`, `bgg_relaxed_accepts`,
`desc_growth_events` / `desc_shrink_events`) and through `mxr_dump()`.

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
      → Region boundaries:      "4-8%,32-10%,64-10%,128-12%,256-10%,512-20%,1024-0%"
      → IRAM fb region layout:  "4-8%,32-10%,64-10%,128-12%,256-10%,512-20%,1024-0%"
      → IRAM fallback order:    DRAM first (recommended)
      → Enable IRAM heap: [*]
      → IRAM_RESERVE_BYTES: 2048
      → Descriptor table placement:  DRAM (.bss)         ← or IRAM (.iram0.text)
      → Allocator state placement:   IRAM — variant B    ← only when tables are in IRAM
                                      (or DRAM — variant A)
```

**3. Build & flash**

```bash
idf.py build flash monitor
```

At boot you will see:

```
I (123) mxr_malloc: init ok: base=0x3ffe9a10 bytes=78832 dram_desc=256 iram_desc=128
I (126) mxr_malloc: IRAM heap ok: base=0x4010a230 bytes=7472 fb_zone=5424 fb_regions=7
```

## ⚙️ Configuration

### Region layout

Both DRAM and IRAM-fallback topologies are defined by one string each:

```
CONFIG_MXR_REGION_CONFIG="4-8%,32-10%,64-10%,128-12%,256-10%,512-20%,1024-0%"
CONFIG_MXR_IRAM_FALLBACK_REGION_CONFIG="4-8%,32-10%,64-10%,128-12%,256-10%,512-20%,1024-0%"
```

| Entry | Meaning |
| --- | --- |
| `4-8%` | Region 0: blocks ≥ 4 bytes, gets 8% of the zone |
| `32-10%` | Region 1: blocks ≥ 32 bytes, gets 10% |
| `1024-0%` | Last region: unlimited, absorbs all leftover memory |

- Rules: the last region is always unlimited and absorbs leftover memory;
- boundaries must be strictly increasing; percent sum ≤ 100; an empty IRAM-fb
  string yields a single flat fallback region. Validated at CMake configure
  time (IRAM-fb string is validated only when `MXR_IRAM_FALLBACK_ENABLED=y`).
- With `MXR_COMPACT_TYPES=y` boundaries must stay ≤ 65535 (CMake checks this),
  because `max_bytes` is packed into `uint16_t`. In variant B the placed
  structs use 32-bit fields, so this check is intentionally conservative.

### Key options

| Option | Default | Description |
| --- | --- | --- |
| `MXR_MAX_DESC` | 256 | Max DRAM descriptors |
| `MXR_IRAM_MAX_DESC` | 128 | Max IRAM descriptors |
| `MXR_COMPACT_TYPES` | y | `uint16_t` for caps/min/max/count (ignored for state structs in variant B) |
| `MXR_USE_IRAM` | y | Enable IRAM heap |
| `MXR_IRAM_RESERVE_BYTES` | 2048 | Hard-bound EXEC zone `[0, reserve)`; `0` disables EXEC |
| `MXR_IRAM_FALLBACK_ENABLED` | y | Enable non-EXEC 32-bit/INTERNAL fallback into IRAM |
| `MXR_IRAM_FALLBACK_MAX_BYTES` | 0 (∞) | Max non-EXEC block allowed in IRAM fb |
| `MXR_IRAM_FALLBACK_REGION_CONFIG` | 7-class string | IRAM fb region layout |
| `MXR_IRAM_FB_ORDER_*` | `DRAM_FIRST` | IRAM-first vs DRAM-first fallback order |
| `MXR_IRAM_EXEC_WHOLE_IF_NO_FB` | y | Give whole IRAM to EXEC when fallback off |
| `MXR_CROSS_REGION_FALLBACK` | y | Cross-region master switch |
| `MXR_DRAM_CROSS_ENABLED` | y | DRAM cross-region on/off |
| `MXR_IRAM_CROSS_ENABLED` | y | IRAM fb cross-region on/off |
| `MXR_ANTI_SLIVER` | y | Absorb tiny leftover gaps |
| `MXR_MIN_SLICE_BYTES` | 8 | Anti-sliver threshold (4–64) |
| `MXR_BEST_FIT_EARLY_EXIT` | y | Best-fit early exit |
| `MXR_BEST_FIT_WASTE_SHIFT` | 2 | Early-exit waste threshold (1–4) |
| `MXR_IRAM_HOT_PATH_DISABLED` | n | Keep malloc/free out of IRAM |
| `MXR_IRAM_PATH_CORE` | y | IRAM hot path = malloc/free only |
| `MXR_IRAM_PATH_ALLOC_FAMILY` | n | IRAM hot path = +calloc/zalloc/realloc |

### Placement: descriptor tables and allocator state

| Option | Effect |
| --- | --- |
| `MXR_DESC_IN_DRAM` | Descriptor tables in `.bss` — default, safest |
| `MXR_DESC_IN_IRAM_TEXT` | Tables in `.iram0.text` — no linker patch needed |
| `MXR_DESC_IN_IRAM_BSS` | Tables in SDK `.iram0.bss` — automatically mapped by `ld/mxr_sections.lf` in the component CMake build |
| `MXR_STATE_IN_IRAM` | Variant B: `s_stats` / `s_region[]` / `s_iram_fb_region[]` stay in IRAM with **all-32-bit fields** |
| `MXR_STATE_IN_DRAM` | Variant A: those objects have no IRAM attribute → `.bss`, natural field widths |

The state placement choice exists **only** when the tables are in IRAM, and
`MXR_STATE_IN_IRAM` without IRAM tables is a compile-time `#error`. See
[IRAM safety](#-iram-safety) for the byte counts and the reasoning.

> Static (non-arena) footprint with default options: tables
> `MXR_MAX_DESC × 8` (+ `MXR_IRAM_MAX_DESC × 8`), state
> `232 + 44 × (N + M)` B in variant A or `256 + 48 × (N + M)` B in variant B,
> where `N`/`M` are DRAM/fallback region counts. Enable
> `MXR_DESC_DYNAMIC` to move the DRAM table into the heap tail instead.

## 🔌 Integration Modes

**Wrap mode (default, recommended)** — original `heap` component stays in the
build; linker `--wrap` redirects calls. Pros: no SDK source patch, easy to disable. Validate wrapping/linkage in the final ELF.

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
size_t heap_caps_get_total_size(uint32_t caps);
size_t heap_caps_get_allocated_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
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
size_t mxr_get_total_size_caps(uint32_t caps);
size_t mxr_get_largest_free_block_caps(uint32_t caps);
size_t mxr_get_allocated_size_caps(uint32_t caps);
void   mxr_dump(void);
```

### Capability bits

```c
MALLOC_CAP_EXEC      (1 << 0)   // executable (IRAM EXEC zone only)
MALLOC_CAP_32BIT     (1 << 1)   // 32-bit aligned access
MALLOC_CAP_8BIT      (1 << 2)   // 8-bit access (DRAM only)
MALLOC_CAP_DMA       (1 << 3)   // DMA-capable (DRAM only)
MALLOC_CAP_SPIRAM    (1 << 10)  // compatibility
MALLOC_CAP_INTERNAL  (1 << 11)  // internal memory (DRAM or IRAM fb)
MXR_CAP_PREFER_IRAM  (1u << 20) // MxR-only SOFT placement hint: try IRAM fb first
```

> **IRAM fallback admission:** a request enters the IRAM fallback path when
> it has `32BIT` **or** `INTERNAL` **or** `caps == 0`, and does **not** have
> `8BIT`, `DMA`, `SPIRAM`, or `EXEC`.

> **`MXR_CAP_PREFER_IRAM`** is a preference, not a requirement: the bit is
> stripped before region matching, so it can never make an allocation fail —
> if IRAM cannot serve it, the request falls through to DRAM as usual
> (`prefer_iram_hits` / `prefer_iram_misses` count both outcomes). It is
> ignored when the IRAM heap or the fallback zone is disabled, and it is
> **not** understood by the stock SDK heap (there it makes the request
> unsatisfiable) — set it only in builds where MxR-Malloc is linked, and only
> for buffers that are accessed exclusively with 32-bit loads/stores.

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
I mxr_malloc: exec=3 iram_fb=2 prefer_iram=1/0 cross=0 cross_skip=0 guard_rej=0 caps_skip=0 free_skip=0 cache_skip=0
I mxr_malloc: insert_fail: bounds=0 overlap=0 dup=0 table_full=0
I mxr_malloc: region_init=ok iram_fb_init=ok
I mxr_malloc: DRAM frag: pct=12% gaps=6 slivers=2(33%) bf_early=31 anti_sliver=2
I mxr_malloc: DRAM: base=0x3ffe9a10 total=78832 free=71200 min_free=68400
I mxr_malloc: IRAM: base=0x4010a230 total=7472 free=5424 min_free=5424 fb_zone=5424 exec_zone=2048 exec_free=1920 exec_rejects=0
I mxr_malloc: iram_fb 0: start=2048 total=5424 min=4 max=-1 free=5424 min_free=5424 largest=5424 alloc=2
I mxr_malloc: region 0: caps=0x0000080e start=0     total=9456  min=4    max=127  ...
I mxr_malloc: region 1: caps=0x0000080e start=9456  total=11032 min=128  max=255  ...
I mxr_malloc: region 2: caps=0x0000080e start=20488 total=7880  min=256  max=511  ...
I mxr_malloc: region 3: caps=0x0000080e start=28368 total=19708 min=512  max=1279 ...
I mxr_malloc: region 4: caps=0x0000080e start=48076 total=30756 min=1280 max=-1   ...
I mxr_malloc: stats: fail_mem=0 fail_table=0 invalid_free=0
```

### Notable counters (`mxr_status_t`)

| Field | Meaning |
| --- | --- |
| `exec_allocs` | EXEC allocations served from the EXEC zone |
| `exec_zone_rejects` | EXEC requests rejected (zone empty / block too large) |
| `iram_fallback_allocs` | Non-EXEC 32-bit allocations placed in IRAM |
| `prefer_iram_hits` / `prefer_iram_misses` | `MXR_CAP_PREFER_IRAM` requests served by IRAM / bounced to DRAM |
| `cross_region_allocs` | Cross-region placements |
| `cross_region_guard_rejects` | Cross-region attempts rejected by GUARD / min_bytes guard |
| `cross_region_skip_fragmented` | Cross-region scans that found no gap |
| `fragmentation_pct` | `(free − largest) / free × 100` |
| `gap_count` / `sliver_count` | Free gaps / gaps below `MXR_MIN_SLICE_BYTES` |
| `best_fit_early_exits` | Best-fit searches that stopped early |
| `anti_sliver_expansions` | Blocks expanded to absorb a sliver |
| `quick_fit_hint_hits` / `_misses` | O(1) hint served the query / had to fall back to a search |
| `bgg_relaxed_accepts` | Big-gap guard relaxed a too-small split |
| `canary_violations` | Blocks whose canary was found damaged (debug builds) |
| `double_free_detects` | Repeated frees caught by the DFD ring |
| `desc_growth_events` / `desc_shrink_events` | Dynamic descriptor table resizes |
| `iram_exec_zone_total/free/min_free` | EXEC zone capacity / free / low-water mark |

## ⚠️ Known Limitations

- **ESP8266 only** — arena limited to ~128 KB by the 31-bit offset field.
- **No heap tracing** — `CONFIG_HEAP_TRACING` is incompatible with wrap mode.
  CMake will emit a warning if both are enabled.
- **IRAM access width** — the hardware allows 32-bit accesses only. MxR-Malloc
  keeps the rule by construction (state is either in DRAM, or all-32-bit in
  IRAM), but any *new* object you add with `MXR_IRAM_DATA_ATTR` must be
  word-only. Run `tools/check_iram_widths.py` in CI; in variant B the
  `_Static_assert` barrier also fails the build. Note that 8/16-bit accesses
  that do slip through will not crash — the SDK handler emulates them at the
  cost of a full exception each.
- **Variant B ignores `COMPACT_TYPES` for `mxr_region_t` / `mxr_status_t`** —
  that is where its extra ~50–70 B of IRAM goes. Variant A keeps the compact
  widths but pays in DRAM.
- **The `≤ 65535` boundary check is conservative** — with `MXR_COMPACT_TYPES=y`
  CMake rejects region boundaries above 65535 even when variant B would allow
  them.
- **`MXR_CANARY` costs 8 bytes per block** and inflates the cluster size of
  every size class; with an aggressive small-class layout (e.g. only 8% of the
  arena below 128 B) the small region can be starved and allocations start
  failing. Use it for debugging, not in production firmware, and re-check your
  region percentages when you enable it.
- **`MXR_DOUBLE_FREE_DETECT` is heuristic** — the ring catches recent repeats;
  a pointer freed long ago and re-freed later may still slip past.
- **No in-place `realloc` across regions** — the block is moved if it does not
  fit.
- **EXEC is DRAM-invisible** — `MALLOC_CAP_EXEC` never falls back to DRAM and
  never leaves the EXEC zone.
- **`MXR_IRAM_PATH_ALLOC_FAMILY`** — placing the full allocation family in IRAM
  consumes significant IRAM. Check `idf.py size` before enabling.
- **IRAM fb order** — with the default `DRAM_FIRST`, pure-32-bit/INTERNAL
  allocations reach IRAM only after DRAM is exhausted. Use `IRAM_FIRST`
  (original behavior) or the per-call `MXR_CAP_PREFER_IRAM` hint if you want
  IRAM preferred.
- **IRAM fb region_for_size** — no implicit fallback to first/last region;
  a misconfigured layout that leaves a size hole will return `NULL` for
  that size class instead of silently using a wrong region.
- **`MXR_DESC_DYNAMIC` requires tables in DRAM** — it carves the table from
  the heap tail, so it is unavailable with `MXR_DESC_IN_IRAM_*`.

## 📁 Project Structure

```
MXR-MALLOC/
├── README.md
├── LICENSE
├── index.html                    # interactive simulator (same Kconfig model)
├── logo-full.svg
├── logo-full-light.svg
└── mxr_malloc/
    ├── CMakeLists.txt            # Build system, region validation, linker wraps
    ├── Kconfig.projbuild         # All configuration options
    ├── include/
    │   └── mxr_malloc.h          # Public API, status/region types, IRAM barriers
    ├── ld/
    │   └── mxr_sections.lf       # Linker-script template (.iram0.bss support)
    ├── mxr_malloc.c              # Core allocator
    ├── mxr_heap_wrap.c           # Linker --wrap integration layer
    ├── mxr_heap_compat.c         # Direct heap_caps_* replacement
    └── mxr_heap_port.c           # Standard libc replacement
```

`tools/check_iram_widths.py` is the optional IRAM-width audit used in CI (see
[IRAM safety](#-iram-safety)).

## 🧪 Testing

### On target

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

    // Soft placement hint — IRAM first, DRAM as fallback
    void *audio = mxr_malloc_caps(1024, MALLOC_CAP_32BIT | MXR_CAP_PREFER_IRAM);

    mxr_dump();
}
```

### In the browser

`index.html` is a full interactive simulator of the allocator (region geometry,
cross-region guards, quick-fit hint, binning, canary, DFD, dynamic descriptor
tables, and the placement choices above). Open it in a browser, or serve it:

```bash
python3 -m http.server 8000   # then http://localhost:8000/index.html
```

Its settings panel mirrors `Kconfig.projbuild`, including
**Descriptor table placement** and **Allocator state placement** (A/B), and can
subtract the static state/tables from the arena budgets so the simulated arena
matches a real build.

### Static audit

```bash
python3 tools/check_iram_widths.py mxr_malloc/
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
