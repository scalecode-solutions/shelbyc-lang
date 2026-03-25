# Packed Sentinel Encoding

**Date:** March 25, 2026
**Status:** Design — no code changes yet
**Depends on:** trit packing (proven in C bootstrap), pentit packing (proven in C bootstrap)

---

## The Insight

Packed trit and pentit arrays don't use the full byte range. The unused values aren't wasted — they're sentinel space. The sentinels are defined as OFFSETS from the end of valid data, so the same sentinel meanings work across both formats with one codepath.

| Encoding | Valid range | Sentinel start | Sentinel slots | Efficiency |
|----------|-----------|---------------|----------------|------------|
| Trit (5 per byte) | 0-242 (3^5=243) | 243 | 13 | 95% |
| Pentit (3 per byte) | 0-124 (5^3=125) | 125 | 131 | 49% |

---

## Offset-Based Sentinel Design

Sentinels are defined as offsets from `max_valid + 1`. The first 6 offsets are shared — same meaning, same order, in both trit and pentit. Everything after offset 6 is format-specific.

```c
#define TRIT_MAX_VALID   242
#define PENTIT_MAX_VALID 124

// Shared sentinel offsets (work on both formats)
#define SENTINEL_SPARSE     1   // all digits in group are zero — skip
#define SENTINEL_UNINIT     2   // never written — poison value
#define SENTINEL_MASKED     3   // don't-care — intentionally skipped
#define SENTINEL_TOMBSTONE  4   // logically deleted
#define SENTINEL_OVERFLOW   5   // computation overflowed digit range
#define SENTINEL_END        6   // end-of-array — stop iteration
```

### Absolute byte values

| Offset | Meaning | Trit byte | Pentit byte |
|--------|---------|-----------|-------------|
| +1 | SPARSE | 243 | 125 |
| +2 | UNINIT | 244 | 126 |
| +3 | MASKED | 245 | 127 |
| +4 | TOMBSTONE | 246 | 128 |
| +5 | OVERFLOW | 247 | 129 |
| +6 | END | 248 | 130 |

### Trit remaining (7 values: 249-255)

| Offset | Trit byte | Status |
|--------|-----------|--------|
| +7 | 249 | reserved |
| +8 | 250 | reserved |
| +9 | 251 | reserved |
| +10 | 252 | reserved |
| +11 | 253 | reserved |
| +12 | 254 | reserved |
| +13 | 255 | CORRUPT (all bits set — maximum corruption signal) |

### Pentit remaining (125 values: 131-255)

| Offset range | Pentit bytes | Purpose |
|-------------|-------------|---------|
| +7 to +8 | 131-132 | ALL_NEG / ALL_POS fast sentinels |
| +9 to +120 | 133-244 | Application-defined metadata (112 values) |
| +121 to +130 | 245-254 | Reserved for future shared sentinels |
| +131 | 255 | CORRUPT (same as trit — all bits set) |

CORRUPT is always 255 in both formats. All bits set. Maximum distance from valid data. Easiest to detect.

---

## The Unified Check

One function handles both trit and pentit:

```c
typedef enum {
    PACKED_VALID,
    PACKED_SPARSE,
    PACKED_UNINIT,
    PACKED_MASKED,
    PACKED_TOMBSTONE,
    PACKED_OVERFLOW,
    PACKED_END,
    PACKED_CORRUPT,
    PACKED_METADATA,     // pentit-only: application metadata
    PACKED_RESERVED,
} PackedStatus;

static inline PackedStatus packed_check(uint8_t byte, uint8_t max_valid) {
    if (byte <= max_valid) return PACKED_VALID;
    if (byte == 255) return PACKED_CORRUPT;

    int offset = byte - max_valid;
    switch (offset) {
    case 1: return PACKED_SPARSE;
    case 2: return PACKED_UNINIT;
    case 3: return PACKED_MASKED;
    case 4: return PACKED_TOMBSTONE;
    case 5: return PACKED_OVERFLOW;
    case 6: return PACKED_END;
    default: return PACKED_METADATA;  // pentit-only range (or reserved for trit)
    }
}

// Usage:
PackedStatus s;
s = packed_check(byte, TRIT_MAX_VALID);    // for trit arrays
s = packed_check(byte, PENTIT_MAX_VALID);  // for pentit arrays
```

One function. One switch. The `max_valid` parameter is the only difference between trit and pentit. The sentinel offsets are the same.

---

## Corruption Detection

### Detection rates

| Format | Invalid bytes | Detection rate (random byte corruption) |
|--------|-------------|----------------------------------------|
| Trit | 13 out of 256 | 5.1% |
| Pentit | 131 out of 256 | 51.2% |
| INT4 | 0 out of 256 | 0% |
| INT8 | 0 out of 256 | 0% |

### The 255 check

Both formats use 255 as CORRUPT. All bits set. One check works everywhere:

```c
if (byte == 255) trap();  // corruption, always, both formats
```

This is the cheapest possible check — one comparison, catches the most common corruption pattern (memory filled with 0xFF from failed erase, wild DMA, etc.).

### Full corruption detection

Any byte value above max_valid that isn't a defined sentinel is corruption:

```c
if (byte > max_valid) {
    int offset = byte - max_valid;
    if (offset > 6 && byte != 255) {
        // For trit: offsets 7-12 are reserved, 13 is CORRUPT
        //   → any value 249-254 in trit is likely corruption
        // For pentit: offsets 7-8 are fast sentinels, 9-120 are metadata
        //   → only values outside defined metadata ranges are corruption
    }
}
```

---

## Sparsity Acceleration

The SPARSE sentinel (offset +1) enables skipping zero groups in inference:

```c
// Works on both trit and pentit — same offset check
void dot_sparse(const uint8_t *weights, const int8_t *input,
                int32_t *output, int64_t M, int64_t K,
                uint8_t max_valid, int digits_per_byte) {
    int64_t groups = (K + digits_per_byte - 1) / digits_per_byte;

    for (int64_t i = 0; i < M; i++) {
        int32_t acc = 0;
        for (int64_t g = 0; g < groups; g++) {
            uint8_t packed = weights[i * groups + g];

            // Sentinel check — works for both trit and pentit
            if (packed > max_valid) {
                int offset = packed - max_valid;
                if (offset == SENTINEL_SPARSE) continue;  // skip zeros
                if (offset == SENTINEL_END) break;         // done
                if (packed == 255) trap();                  // corrupt
                continue;                                   // other sentinels: skip
            }

            // Normal extraction — format-specific
            // (trit uses pow3, pentit uses pow5)
            acc += extract_and_dot(packed, input, g, digits_per_byte);
        }
        output[i] = acc;
    }
}
```

### Impact on sparse models

In a model with 70% zero weights:
- Without sentinels: process all groups, multiply by zero — wasted cycles
- With SPARSE sentinel: 70% of groups skipped with one comparison
- Effective speedup: ~2-3x on sparse models, zero overhead on dense models

---

## Pentit Fast Sentinels

Pentit has extra sentinel space. Offsets +7 and +8 (bytes 131-132) are fast sentinels for common patterns:

| Offset | Pentit byte | Name | Meaning |
|--------|-------------|------|---------|
| +7 | 131 | ALL_NEG | all 3 pentits are negative (N1 or N2) |
| +8 | 132 | ALL_POS | all 3 pentits are positive (P1 or P2) |

These enable fast-path inference when an entire group has the same sign:

```c
if (packed == 131) {
    // ALL_NEG: subtract all inputs (exact magnitudes unknown but all negative)
    // Fast approximation: subtract each input once (assumes N1)
    for (int p = 0; p < 3; p++) acc -= input[base_k + p];
    continue;
}
if (packed == 132) {
    // ALL_POS: add all inputs
    for (int p = 0; p < 3; p++) acc += input[base_k + p];
    continue;
}
```

---

## Pentit Application Metadata (Offsets +9 to +120)

112 byte values (133-244) available for application-defined metadata embedded in pentit weight arrays:

| Use case | Encoding | Example |
|----------|----------|---------|
| Layer boundary | 133 + layer_id | marks where layers start/end in a flat weight array |
| Quantization scale | 133 + scale_idx | index into scale factor table |
| Attention head | 133 + head_id | attention head boundaries |
| Pruning marker | 200 | structurally pruned — not just zero, architecturally removed |
| Precision gate | 201-203 | trit=201, pentit=202, INT4=203 — dynamic precision marker |

The precision gate markers (201-203) enable the trit-gated dynamic precision model: the weight array itself contains markers indicating which precision level each section uses.

---

## Self-Delimiting Arrays

The END sentinel (offset +6) makes packed arrays self-delimiting:

```c
// Iterate until END — no length field needed
for (int64_t i = 0; ; i++) {
    uint8_t packed = data[i];
    int offset = packed - max_valid;
    if (offset == SENTINEL_END) break;
    // process group
}
```

Works on both trit and pentit. Useful for:
- Variable-length trit/pentit data in network protocols
- Streaming model weight loading
- Self-describing file format sections

---

## Byte Layout Summary

```
TRIT:
 0 ──────────── valid (243 values) ──────────── 242 │243│244│245│246│247│248│249 ... 254│255│
                                                     SPR UNI MSK TMB OVF END (reserved) CRP
                                                     ├── shared sentinels ──┤

PENTIT:
 0 ── valid (125 values) ── 124 │125│126│127│128│129│130│131│132│133 ──── 244│245 ... 254│255│
                                 SPR UNI MSK TMB OVF END NEG POS  (app metadata)  (reserved) CRP
                                 ├── shared sentinels ──┤ (pentit-only)
```

Both formats:
- Valid data starts at 0
- Shared sentinels start at max_valid + 1
- Same 6 sentinels in the same order at the same offsets
- CORRUPT is always 255
- One codepath handles both with `max_valid` as the only parameter

---

## Connection to the Language

The sentinel system is invisible to normal code. `[10]pentit` is an array. The packing and sentinels are implementation details.

Where sentinels surface:
- **Debug builds**: corruption detection on every extraction — zero cost since the byte is already loaded
- **Flow ownership**: UNINIT sentinel verifies definite initialization of packed arrays
- **Neural inference**: SPARSE sentinel accelerates sparse models
- **Model format**: application metadata embeds model structure in the weight stream
- **Dynamic precision**: precision gate markers enable trit-gated adaptive inference

The offset-based design means any future packed digit type (sevit, nonit, whatever) gets the same sentinels automatically. Define `max_valid`, and offsets +1 through +6 work.

---

*The packing inefficiency isn't waste. It's a sentinel budget. Trit gets 13 values — enough for the 6 shared sentinels and corruption detection. Pentit gets 131 values — enough for shared sentinels, fast sentinels, 112 metadata values, and 51% corruption detection. The offset-based design ensures one codepath handles all packed digit formats. The encoding IS the error detection.*
