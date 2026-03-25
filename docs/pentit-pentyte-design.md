# Pentit / Pentyte Design

**Date:** March 24, 2026
**Status:** Design exploration — no code changes yet
**Depends on:** trit/tryte primitives (proven in C bootstrap)

---

## What It Is

A five-valued balanced quinary digit. The third scalar logic primitive alongside bit and trit.

| Primitive | Base | Values | Bits per digit | Origin |
|-----------|------|--------|----------------|--------|
| bit | 2 (binary) | 0, 1 | 1.00 | Every computer ever |
| trit | 3 (balanced ternary) | N(-1), Z(0), P(+1) | 1.58 | Soviet Setun (1958), BitNet 1.58b |
| pentit | 5 (balanced quinary) | N2(-2), N1(-1), Z(0), P1(+1), P2(+2) | 2.32 | ShelbyC |

Naming follows the established pattern:
- **bit** — **bi**nary dig**it**
- **trit** — **tri**nary dig**it**
- **pentit** — **pent**ary dig**it**

Packed form:
- **byte** — 8 bits
- **tryte** — 9 trits (packed as 2 bytes, 3^9 = 19,683 states)
- **pentyte** — 5 pentits (packed as 2 bytes, 5^5 = 3,125 states)

---

## The Five Values

```
pentit.N2    -2    strong negative
pentit.N1    -1    negative
pentit.Z      0    zero / neutral
pentit.P1    +1    positive
pentit.P2    +2    strong positive
```

Symmetric around zero. Ordered. The absolute value encodes intensity. The sign encodes direction.

---

## Packing

3 pentits per byte (5^3 = 125, fits in 7 bits of a u8):

```
Byte layout: [p2][p1][p0] where value = p0 + p1*5 + p2*25
Range: 0-124 (125 valid values out of 256)
```

Extraction:
```c
static const uint8_t pow5[3] = {1, 5, 25};

int8_t pentit_extract(uint8_t packed, int pos) {
    uint8_t raw = (packed / pow5[pos]) % 5;
    return (int8_t)raw - 2;  // 0→-2, 1→-1, 2→0, 3→+1, 4→+2
}

uint8_t pentit_pack(int8_t p0, int8_t p1, int8_t p2) {
    return (uint8_t)((p0 + 2) + (p1 + 2) * 5 + (p2 + 2) * 25);
}
```

This mirrors the trit packing (5 trits per byte, 3^5 = 243) but with different density. Trits pack more densely (5 per byte vs 3 per byte) because they have fewer values per digit.

| Type | Per byte | States per byte | Efficiency |
|------|----------|----------------|------------|
| bit | 8 | 256 | 100% |
| trit | 5 | 243 | 95% |
| pentit | 3 | 125 | 49% |

Pentit packing wastes ~51% of the byte. For large arrays, a denser packing could be used: 11 pentits per 4 bytes (5^11 = 48,828,125, fits in 26 bits). But 3-per-byte is simpler and matches the trit model.

---

## Operators

Following the Cottrell Confluence pattern for trit, extended to five values.

### Arithmetic

| Op | Symbol | Definition | Example |
|----|--------|-----------|---------|
| Add | `+` | mod-5 balanced | P1 + P1 = P2, P2 + P1 = N2 (wraps) |
| Multiply | `*` | balanced multiply, clamp to [-2,+2] | P1 * N1 = N1, P2 * N1 = N2 |
| Negate | `~` | negate | ~P2 = N2, ~Z = Z |
| Abs | `abs` | absolute value | abs(N2) = P2, abs(Z) = Z |

### Logic (min/max on ordered values)

| Op | Symbol | Definition | Example |
|----|--------|-----------|---------|
| Min (AND) | `&` | min(a, b) | P1 & N1 = N1, P2 & Z = Z |
| Max (OR) | `\|` | max(a, b) | P1 \| N1 = P1, N2 \| Z = Z |
| Consensus | `**` | agree or neutral | P1 ** P1 = P1, P1 ** N1 = Z |
| Clamp | `clamp` | restrict range | clamp(P2, N1, P1) = P1 |

### Comparison

| Op | Symbol | Result type | Example |
|----|--------|------------|---------|
| Spaceship | `<=>` | pentit | (3 <=> 1) = P2, (3 <=> 3) = Z, (1 <=> 3) = N2 |

The spaceship operator returning a pentit instead of a trit gives intensity: not just "less/equal/greater" but "much less / slightly less / equal / slightly greater / much greater." Useful for fuzzy comparison, scoring, and ranking.

---

## Use Cases

### 1. Neural Inference Weights

BitNet 1.58b uses ternary weights {-1, 0, +1}. Pentit weights {-2, -1, 0, +1, +2} give two levels of intensity per direction:

```
Weight = P2(+2): add input twice      (strong activation)
Weight = P1(+1): add input            (normal activation)
Weight = Z(0):   skip                 (no connection)
Weight = N1(-1): subtract input       (inhibition)
Weight = N2(-2): subtract input twice (strong inhibition)
```

Still no multiplier needed. P2 is a left shift. N2 is a left shift + negate. The hardware path is table lookup and accumulate, same as trit inference but with finer granularity.

Information per weight: 2.32 bits. Sits between BitNet's 1.58 bits (trit) and INT4's 4 bits. A sweet spot between compression and expressiveness.

```
fn PentitDot(weights [K]pentit, input [K]i8) i32 {
    acc := 0;
    i := 0;
    while i < K {
        w := weights[i];
        v := input[i];
        if w == pentit.P2      { acc = acc + v + v; }
        else if w == pentit.P1 { acc = acc + v; }
        else if w == pentit.N1 { acc = acc - v; }
        else if w == pentit.N2 { acc = acc - v - v; }
        // Z: skip
        i = i + 1;
    }
    return acc;
}
```

The LUT approach also works: for each group of 3 packed pentits (one byte), precompute a 125-entry lookup table of accumulated values. One table lookup per byte of weights.

### 2. Ownership State

The five ownership states map to pentit values with meaningful ordering:

```
pentit.P2  →  frozen/immutable   (most alive — shareable, guaranteed to exist)
pentit.P1  →  alive/owned        (standard exclusive ownership)
pentit.Z   →  borrowed           (neutral — temporarily lent, will return)
pentit.N1  →  consumed/moved     (gone, potentially recoverable via restart)
pentit.N2  →  dropped/destroyed  (most dead — gone permanently)
```

The ordering matters: `frozen > alive > borrowed > consumed > dropped`. The merge function at control flow join points becomes: take the minimum (most conservative).

```
// Join point merge: conservative minimum
fn OwnershipMerge(a pentit, b pentit) pentit {
    if a == b { return a; }
    return min(a, b);
}

// P2 merge P1 = P1 (conservatively: owned, not frozen)
// P1 merge Z  = Z  (conservatively: borrowed)
// P1 merge N1 = conflict (alive on one path, consumed on another)
// Z  merge N1 = conflict
```

Conflicts (where one path has a positive value and another has a negative value) are the "maybe consumed" case — the flow analysis reports a warning or error.

### 3. Confidence / Signal Strength

Five-valued logic naturally encodes confidence:

```
pentit.P2  →  certain yes     (proven true)
pentit.P1  →  likely yes      (probably true)
pentit.Z   →  unknown         (no information)
pentit.N1  →  likely no       (probably false)
pentit.N2  →  certain no      (proven false)
```

Applications:
- Sensor fusion: combine readings with different confidence levels
- Decision systems: "should we retry?" isn't yes/no, it's a confidence spectrum
- Cache validity: certain-valid / probably-valid / unknown / probably-stale / certain-stale
- Network health: healthy / degraded / unknown / failing / down

### 4. Priority and Severity

```
pentit.P2  →  critical
pentit.P1  →  high
pentit.Z   →  normal
pentit.N1  →  low
pentit.N2  →  minimal
```

The arithmetic works: `critical & high = high` (min). `low | normal = normal` (max). Priority composition falls out of pentit operators.

### 5. Extended Spaceship Comparison

A regular spaceship `<=>` returns a trit: less/equal/greater. An extended comparison could return a pentit with magnitude:

```
fn Compare(a f64, b f64, epsilon f64) pentit {
    diff := a - b;
    if diff > epsilon * 10.0  { return pentit.P2; }  // much greater
    if diff > epsilon         { return pentit.P1; }  // slightly greater
    if diff > -epsilon        { return pentit.Z;  }  // approximately equal
    if diff > -epsilon * 10.0 { return pentit.N1; }  // slightly less
    return pentit.N2;                                  // much less
}
```

Useful for sorting stability, fuzzy matching, and approximate algorithms where "how different" matters, not just "which direction."

---

## LLVM Representation

A `pentit` is stored as `i8` at the LLVM level, with range [-2, +2]. Same as `trit` being stored as `i8` with range [-1, +1].

```c
case TY_PENTIT:
    return LLVMInt8TypeInContext(cg->ctx);
```

Range checking inserted at assignment:
```llvm
; pentit range check: -2 <= val <= 2
%in_range = icmp sle i8 %val, 2
%in_range2 = icmp sge i8 %val, -2
%valid = and i1 %in_range, %in_range2
br i1 %valid, label %ok, label %trap
```

Packed pentit arrays use the 3-per-byte packing described above, identical in structure to trit's 5-per-byte packing but with different constants.

---

## Pentit Tile

Following the TritTile pattern:

```
primitive PentitTile<M, K> {
    weights [M][K]pentit;
    input   [K]i8;
}

fn (t &PentitTile<M, K>) Infer() [M]i32 {
    return t.weights.Dot(t.input);
}
```

The dot product for pentit weights:
- P2 × input = input << 1 (shift left = multiply by 2)
- P1 × input = input
- Z × input = 0
- N1 × input = -input
- N2 × input = -(input << 1)

All shifts and adds. No multiplier. The LUT approach: 5^3 = 125 entries per byte group (vs 3^5 = 243 for trit). Smaller tables, fewer entries to precompute.

SIMD lowering is identical to trit tiles:
- ARM NEON: TBL (table lookup) + accumulate
- x86 AVX2: VPSHUFB (shuffle) + accumulate

The table contents differ (125 entries vs 243) but the instruction sequence is the same.

---

## Relationship to Trit

Pentit is NOT a replacement for trit. They serve different cardinalities:

| Cardinality | Type | Use when |
|-------------|------|----------|
| 2 | bool | binary decision — yes/no, true/false |
| 3 | trit | three-valued — negative/zero/positive, error/normal/recovered |
| 5 | pentit | five-valued — intensity + direction, ownership states, confidence |
| N | enum | more than five — custom domain-specific values |

The developer picks the type that matches the problem. The compiler picks the representation. Bool is i1. Trit is i8 with range [-1,+1]. Pentit is i8 with range [-2,+2]. Enum is i32 with exhaustiveness checking.

Rules:
- If there are exactly 2 states: use `bool`
- If there are exactly 3 states with a natural center: use `trit`
- If there are exactly 5 states with a natural center and ordering: use `pentit`
- If there are any other number of states: use `enum`
- If the data is continuous: use `f32`/`f64`
- Never force a type that doesn't fit the cardinality

---

## Open Questions

### 1. Cottrell Confluence extension
The Cottrell operators are defined for trit. Do they extend naturally to pentit, or does pentit need its own operator table? The min/max/negate operators extend obviously. Consensus and mod-add need definition for five values.

### 2. Pentit ↔ trit conversion
A pentit can be narrowed to a trit: N2→N, N1→N, Z→Z, P1→P, P2→P. Or with threshold: only N2→N, only P2→P, everything else→Z. Which conversion is the default? Should it require explicit `as trit`?

### 3. Packed pentyte size
A pentyte of 5 pentits = 5^5 = 3,125 states. A pentyte of 9 pentits = 5^9 = 1,953,125 states (needs 21 bits = 3 bytes). Should the pentyte match the tryte in "digit count" (9) or "byte count" (2)?

### 4. Implementation priority
Pentit is less fundamental than trit. The trit system is proven and integrated. Pentit can be added later without changing existing code. The question is whether it should be in the language spec from the start (as a reserved type) or added when a concrete use case demands it.

---

## Implementation Path (When Ready)

Following the trit pattern exactly:

1. Add `TY_PENTIT` and `TY_PENTYTE` to TypeKind
2. Add `KwPentit` and `KwPentyte` to TokenKind
3. Add LLVM type lowering (i8 for pentit, packed byte array for pentyte)
4. Add pack/extract functions in runtime.c
5. Add pentit literal syntax: `pentit.N2`, `pentit.N1`, `pentit.Z`, `pentit.P1`, `pentit.P2`
6. Add operators following the Cottrell extension
7. Add PentitTile with dot product
8. Add SIMD lowering (same instructions as trit, different LUT)
9. Test against Furling Gate — no regressions, new pentit tests pass

Each step builds on the previous. Each step tested. No stubs.

---

*Pentit extends the balanced digit family from binary (2) through ternary (3) to quinary (5). The pattern could continue — a septit (7 values) or nonit (9 values) — but five is the sweet spot for most real-world multi-valued decisions. More than 5 states and you should use an enum.*
