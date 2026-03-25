# Part 6: Tensor Tile — Language Feature + SIMD

**Goal:** Build TritTile as a language feature in the C bootstrap. Scalar implementation, ARM NEON lowering, x86 AVX2 lowering. This is a developer-facing feature for trit math and ternary neural inference — NOT a forced integration with the ownership solver.

**Files modified:** `types.h`, `types.c`, `cg_type.c`, `cg_builtin.c`, `runtime.c`

**Depends on:** Existing trit scalar ops (Cottrell Confluence) and packed trit arrays

---

## What exists today

| Layer | Status | Location |
|-------|--------|----------|
| Scalar trit ops (AND/OR/MUL/CONSENSUS/ADD) | ✅ Done | `cg_expr.c:1704-1737` |
| Packed trit arrays (5 per byte) | ✅ Done | `cg_literal.c:99-139` |
| TritTile type | ❌ Not built | — |
| TritTile.Dot() scalar | ❌ Not built | — |
| ARM NEON vectorized dot | ❌ Not built | — |
| x86 AVX2 vectorized dot | ❌ Not built | — |

---

## What this is NOT

The previous version of this document forced ownership solver effects into a trit matrix. That was wrong. Flow effects have 6 kinds (READ, WRITE, CONSUME, BORROW, BORROW_MUT, DROP). Collapsing them to 3 trit values loses information. The ownership solver processes effects sequentially with a switch statement on EffectKind — that's the correct approach because the inputs aren't trits.

TritTile is for data that IS genuinely ternary:
- Ternary neural network weights (-1, 0, +1)
- Balanced ternary arithmetic on vectors
- Any user domain where three-valued logic applies

The ownership solver and TritTile share:
- The packed trit storage format (5 per byte)
- The SIMD hardware path (TBL on ARM, VPSHUFB on x86)
- The Cottrell Confluence operator semantics

But the solver doesn't call TritTile.Dot(). It calls its own effect propagation loop. They're separate tools that happen to use the same low-level primitives.

---

## Sub-step 6.1: TY_TRIT_TILE type

**File:** `src/types.h`

### Add to TypeKind:
```c
    TY_TRIT_TILE,       /* TritTile<M, K> — M×K packed trit matrix */
```

### Add to Type union:
```c
        /* TY_TRIT_TILE */
        struct {
            int64_t rows;       /* M: output dimension */
            int64_t cols;       /* K: input dimension */
        } trit_tile;
```

### LLVM lowering (cg_type.c):
```c
    case TY_TRIT_TILE: {
        uint64_t total_trits = (uint64_t)t->u.trit_tile.rows * (uint64_t)t->u.trit_tile.cols;
        uint64_t packed_bytes = (total_trits + 4) / 5;
        return LLVMArrayType2(LLVMInt8TypeInContext(cg->ctx), packed_bytes);
    }
```

### Test:
```
fn Main() {
    tile := TritTile<4, 8>.New();  // 4×8 = 32 trits = 7 bytes
    Println(SizeOf(tile));  // 7
}
```

---

## Sub-step 6.2: Scalar TritTile.Dot()

**File:** `src/runtime.c`

```c
/*
 * Trit tile dot product: M×K trit matrix × K-element i8 vector → M-element i32 vector.
 *
 * For each output row i:
 *   result[i] = sum_{k=0}^{K-1} trit_select(weights[i][k], input[k])
 *   where trit_select(N, x) = -x, trit_select(Z, x) = 0, trit_select(P, x) = x
 *
 * Trit encoding: 5 per byte, base-3.
 *   0 = N(-1), 1 = Z(0), 2 = P(+1)
 */
void __sc_trit_tile_dot(
    const uint8_t *weights,
    const int8_t  *input,
    int32_t       *output,
    int64_t        M,
    int64_t        K
) {
    static const uint8_t pow3[5] = {1, 3, 9, 27, 81};

    for (int64_t i = 0; i < M; i++) {
        int32_t acc = 0;
        for (int64_t k = 0; k < K; k++) {
            int64_t flat = i * K + k;
            uint8_t packed = weights[flat / 5];
            uint8_t trit_raw = (packed / pow3[flat % 5]) % 3;

            int8_t in_val = input[k];
            if (trit_raw == 0)      acc -= in_val;   /* N: subtract */
            else if (trit_raw == 2) acc += in_val;   /* P: add */
            /* Z: skip */
        }
        output[i] = acc;
    }
}
```

### Codegen (cg_builtin.c):
```c
/* TritTile<M, K>.Dot(input [K]i8) [M]i32 */
if (type->kind == TY_TRIT_TILE && method_name_eq("Dot")) {
    LLVMValueRef dot_fn = cg_get_or_declare(cg, "__sc_trit_tile_dot", ...);
    LLVMValueRef args[] = { weights_ptr, input_ptr, out_ptr, M_const, K_const };
    LLVMBuildCall2(cg->builder, fn_type, dot_fn, args, 5, "");
    return LLVMBuildLoad2(cg->builder, out_ty, out_alloca, "trit.dot.result");
}
```

### Test:
```
fn Main() {
    // 2×3 tile: [[P, N, Z], [Z, P, N]]
    // Input: [10, 20, 30]
    // Row 0: +10 - 20 + 0 = -10
    // Row 1: 0 + 20 - 30 = -10
    tile := TritTile<2, 3>.From([trit.P, trit.N, trit.Z, trit.Z, trit.P, trit.N]);
    input := [10, 20, 30];
    result := tile.Dot(input);
    Println(result[0]);  // -10
    Println(result[1]);  // -10
}
```

---

## Sub-step 6.3: ARM NEON vectorized dot

**File:** `src/runtime.c` — `#ifdef __aarch64__`

The NEON approach uses a lookup table:
1. For each group of 5 input values, precompute a 243-entry LUT (3^5 = 243)
2. Each LUT entry = the sum of trit_select(trit_i, input_i) for the 5 trits packed in that byte value
3. Process the weight matrix by loading packed bytes and looking up the precomputed sums
4. Accumulate results

```c
#if defined(__aarch64__)
void __sc_trit_tile_dot_neon(
    const uint8_t *weights,
    const int8_t  *input,
    int32_t       *output,
    int64_t        M,
    int64_t        K
) {
    static const uint8_t pow3[5] = {1, 3, 9, 27, 81};
    int64_t num_groups = (K + 4) / 5;

    /* Phase 1: Build LUT for each group of 5 inputs */
    int16_t *luts = (int16_t *)alloca(sizeof(int16_t) * 256 * num_groups);
    for (int64_t g = 0; g < num_groups; g++) {
        int64_t base_k = g * 5;
        int64_t count = (base_k + 5 <= K) ? 5 : K - base_k;
        int8_t inv[5] = {0};
        for (int64_t p = 0; p < count; p++) inv[p] = input[base_k + p];

        for (int v = 0; v < 243; v++) {
            int16_t sum = 0;
            int tmp = v;
            for (int p = 0; p < (int)count; p++) {
                int trit = tmp % 3;
                tmp /= 3;
                if (trit == 0)      sum -= inv[p];
                else if (trit == 2) sum += inv[p];
            }
            luts[g * 256 + v] = sum;
        }
        for (int v = 243; v < 256; v++) luts[g * 256 + v] = 0;
    }

    /* Phase 2: Lookup and accumulate per row */
    for (int64_t i = 0; i < M; i++) {
        int32_t acc = 0;
        for (int64_t g = 0; g < num_groups; g++) {
            int64_t byte_idx = (i * K + g * 5) / 5;
            acc += luts[g * 256 + weights[byte_idx]];
        }
        output[i] = acc;
    }
}
#endif
```

Full NEON vectorization with `vld1q_u8` + `vtbl1_u8` for 16 bytes at once is a further optimization on top of this. The LUT approach is the key insight — it replaces per-trit branching with a single table lookup per packed byte.

### Test: same as 6.2, must produce identical output.

---

## Sub-step 6.4: x86 AVX2 vectorized dot

**File:** `src/runtime.c` — `#ifdef __x86_64__`

Same LUT approach, using `VPSHUFB` (packed shuffle bytes) for the lookup step.

```c
#if defined(__x86_64__)
#include <immintrin.h>

void __sc_trit_tile_dot_avx2(
    const uint8_t *weights,
    const int8_t  *input,
    int32_t       *output,
    int64_t        M,
    int64_t        K
) {
    /* Same LUT-based approach as NEON.
     * VPSHUFB handles 32 lookups in one instruction (AVX2).
     * For LUTs > 16 entries, decompose into nibble lookups. */

    /* ... same Phase 1 LUT building ... */
    /* ... Phase 2 uses _mm256_shuffle_epi8 for vectorized lookup ... */

    /* Scalar fallback for non-AVX2 machines handled by __sc_trit_tile_dot */
}
#endif
```

### Codegen target dispatch:
```c
const char *fn_name;
#if defined(__aarch64__)
    fn_name = "__sc_trit_tile_dot_neon";
#elif defined(__x86_64__)
    fn_name = "__sc_trit_tile_dot_avx2";
#else
    fn_name = "__sc_trit_tile_dot";
#endif
```

### Test: same output, platform optimization only.

---

## Summary

| Sub-step | What | Test |
|----------|------|------|
| 6.1 | TY_TRIT_TILE type + LLVM lowering | SizeOf test |
| 6.2 | Scalar `__sc_trit_tile_dot()` | 2×3 dot product = [-10, -10] |
| 6.3 | ARM NEON LUT-based dot | Same output as 6.2 |
| 6.4 | x86 AVX2 LUT-based dot | Same output as 6.2 |

TritTile is a developer-facing feature. The ownership solver has its own path-filtered effect propagation (Part 5). They share packed trit storage and SIMD hardware but are separate tools for separate problems.
