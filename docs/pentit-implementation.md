# Pentit/Pentyte Implementation Plan

**Date:** March 24, 2026
**Status:** Design complete — ready for implementation
**C Bootstrap:** `/Users/travis/GitHub/ShelbyC/`

---

## What gets built

Pentit follows the EXACT same implementation path as trit. Every file that trit touches, pentit touches. Every test pattern for trit has a pentit equivalent.

---

## Encoding

Pentit values: N2(-2), N1(-1), Z(0), P1(+1), P2(+2)

Stored as `i8` at LLVM level. Internal encoding uses offset-by-2 (unsigned 0-4) for arithmetic:

```
pentit value:   N2(-2)  N1(-1)  Z(0)  P1(+1)  P2(+2)
raw encoding:     0       1      2      3       4
```

This matches trit's encoding: trit N(-1)=0, Z(0)=1, P(+1)=2.

Packing: 3 pentits per byte (5^3 = 125, fits in u8):
```
value = p0 + p1*5 + p2*25    (range 0-124)

static const uint8_t pow5[3] = {1, 5, 25};
```

---

## Operator Truth Tables

All operators defined for every combination. 25 entries each (5×5). Using the raw encoding (0-4, center at 2).

### AND — min(a, b)

Same as trit AND — take the lesser value.

```
      N2  N1   Z  P1  P2
N2  | N2  N2  N2  N2  N2
N1  | N2  N1  N1  N1  N1
 Z  | N2  N1   Z   Z   Z
P1  | N2  N1   Z  P1  P1
P2  | N2  N1   Z  P1  P2
```

LLVM: `select(icmp ult a, b, a, b)` — identical to trit AND codegen.

### OR — max(a, b)

```
      N2  N1   Z  P1  P2
N2  | N2  N1   Z  P1  P2
N1  | N1  N1   Z  P1  P2
 Z  |  Z   Z   Z  P1  P2
P1  | P1  P1  P1  P1  P2
P2  | P2  P2  P2  P2  P2
```

LLVM: `select(icmp ugt a, b, a, b)` — identical to trit OR codegen.

### Multiply — balanced, clamped to [-2,+2]

The product of two balanced values, clamped to the pentit range:

```
real product: a_signed * b_signed, clamped to [-2, +2]

      N2  N1   Z  P1  P2
N2  | P2  P2   Z  N2  N2    (−2×−2=4→clamp→P2, −2×−1=2=P2, etc)
N1  | P2  P1   Z  N1  N2    (−1×−2=2=P2, −1×−1=1=P1, etc)
 Z  |  Z   Z   Z   Z   Z
P1  | N2  N1   Z  P1  P2    (+1×−2=−2=N2, +1×−1=−1=N1, etc)
P2  | N2  N2   Z  P2  P2    (+2×−2=−4→clamp→N2, +2×+2=4→clamp→P2)
```

Note: clamping means P2*P2=P2 (not 4) and N2*N2=P2 (not 4). This preserves the pentit range.

LLVM codegen:
```c
case ROP_PENTIT_MUL: {
    /* Convert from raw (0-4) to signed (-2 to +2) */
    LLVMValueRef two = LLVMConstInt(i8t, 2, false);
    LLVMValueRef a_s = LLVMBuildSub(cg->builder, lhs, two, "a.s");
    LLVMValueRef b_s = LLVMBuildSub(cg->builder, rhs, two, "b.s");
    /* Multiply (result is i8, may overflow range) */
    LLVMValueRef prod = LLVMBuildMul(cg->builder, a_s, b_s, "prod");
    /* Clamp to [-2, +2] */
    LLVMValueRef neg2 = LLVMConstInt(i8t, (uint64_t)-2, true);
    LLVMValueRef pos2 = LLVMConstInt(i8t, 2, true);
    LLVMValueRef clamped_lo = LLVMBuildSelect(cg->builder,
        LLVMBuildICmp(cg->builder, LLVMIntSLT, prod, neg2, ""),
        neg2, prod, "clamp.lo");
    LLVMValueRef clamped = LLVMBuildSelect(cg->builder,
        LLVMBuildICmp(cg->builder, LLVMIntSGT, clamped_lo, pos2, ""),
        pos2, clamped_lo, "clamp.hi");
    /* Convert back to raw (0-4) */
    return LLVMBuildAdd(cg->builder, clamped, two, "pentit.mul.r");
}
```

### Add — mod-5 balanced

`(a_raw + b_raw + 3) % 5` (offset so that Z+Z=Z):

```
      N2  N1   Z  P1  P2
N2  | P1  P2  N2  N1   Z    (wraps: −2+−2=−4→+1=P1 in mod-5)
N1  | P2  N2  N1   Z  P1
 Z  | N2  N1   Z  P1  P2
P1  | N1   Z  P1  P2  N2
P2  |  Z  P1  P2  N2  N1
```

LLVM:
```c
case ROP_PENTIT_ADD: {
    LLVMValueRef sum = LLVMBuildAdd(cg->builder, lhs, rhs, "p.sum");
    LLVMValueRef three = LLVMConstInt(i8t, 3, false);
    LLVMValueRef sum3 = LLVMBuildAdd(cg->builder, sum, three, "p.sum3");
    LLVMValueRef five = LLVMConstInt(i8t, 5, false);
    return LLVMBuildURem(cg->builder, sum3, five, "pentit.add.r");
}
```

### Consensus — agree on non-zero, or Z

Same as trit consensus extended: if both are the same non-zero value, return it. Otherwise Z.

```
      N2  N1   Z  P1  P2
N2  | N2   Z   Z   Z   Z
N1  |  Z  N1   Z   Z   Z
 Z  |  Z   Z   Z   Z   Z
P1  |  Z   Z   Z  P1   Z
P2  |  Z   Z   Z   Z  P2
```

LLVM:
```c
case ROP_PENTIT_CONSENSUS: {
    LLVMValueRef two = LLVMConstInt(i8t, 2, false);  /* Z in raw encoding */
    LLVMValueRef same = LLVMBuildICmp(cg->builder, LLVMIntEQ, lhs, rhs, "same");
    LLVMValueRef nz = LLVMBuildICmp(cg->builder, LLVMIntNE, lhs, two, "nz");
    LLVMValueRef both = LLVMBuildAnd(cg->builder, same, nz, "cons.c");
    return LLVMBuildSelect(cg->builder, both, lhs, two, "pentit.cons.r");
}
```

### Negate — flip sign

```
~N2 = P2, ~N1 = P1, ~Z = Z, ~P1 = N1, ~P2 = N2
```

In raw encoding: `4 - val`

LLVM:
```c
case ROP_PENTIT_INV: {
    LLVMValueRef four = LLVMConstInt(i8t, 4, false);
    return LLVMBuildSub(cg->builder, four, operand, "pentit.inv.r");
}
```

### Equality / Inequality

Same as trit — direct comparison.

```c
case ROP_PENTIT_EQ:
    return LLVMBuildICmp(cg->builder, LLVMIntEQ, lhs, rhs, "pentit.eq");
case ROP_PENTIT_NE:
    return LLVMBuildICmp(cg->builder, LLVMIntNE, lhs, rhs, "pentit.ne");
```

---

## C Bootstrap Changes — File by File

### 1. types.h

**Location:** After TY_TRYTE (line 29)

```c
    TY_TRIT,        /* trit — balanced ternary digit {-1, 0, +1} */
    TY_TRYTE,       /* tryte — 9 trits packed in u16 */
    TY_PENTIT,      /* pentit — balanced quinary digit {-2, -1, 0, +1, +2} */
    TY_PENTYTE,     /* pentyte — 5 pentits packed in u16 */
```

### 2. token.h

**Location:** After TOK_KW_TRYTE (line 90), alphabetically sorted

```c
    TOK_KW_TRIT,
    TOK_KW_TRYTE,
    // ... other keywords ...
    // Add where alphabetically appropriate:
    TOK_KW_PENTIT,
    TOK_KW_PENTYTE,
```

Actually, `pentit` comes before `return` alphabetically. Insert between existing keywords at the right position.

### 3. lexer.c

**Location:** Keyword table (line 21+)

```c
    {"pentit",    TOK_KW_PENTIT},
    {"pentyte",   TOK_KW_PENTYTE},
```

Token-to-string:
```c
    case TOK_KW_PENTIT:          return "pentit";
    case TOK_KW_PENTYTE:         return "pentyte";
```

### 4. cottrell.h

**Location:** After ROP_TRIT_NE (line 84)

```c
    /* ---- Pentit operations ---- */
    ROP_PENTIT_AND,         /* & on pentit: min(a, b) */
    ROP_PENTIT_OR,          /* | on pentit: max(a, b) */
    ROP_PENTIT_MUL,         /* * on pentit: balanced multiply, clamped to [-2,+2] */
    ROP_PENTIT_ADD,         /* + on pentit: mod-5 balanced addition */
    ROP_PENTIT_CONSENSUS,   /* ** on pentit: agree-on-nonzero or Z */
    ROP_PENTIT_INV,         /* ~ on pentit: negate (4 - val in raw encoding) */
    ROP_PENTIT_EQ,          /* == on pentit */
    ROP_PENTIT_NE,          /* != on pentit */
```

### 5. cottrell.c

**Location:** After the `if (lt->kind == TY_TRIT)` block (line 63+)

```c
    /* ---- Pentit: five-valued balanced quinary ---- */
    if (lt->kind == TY_PENTIT) {
        r.result_type = lt;  /* pentit ops return pentit */
        r.valid = true;
        switch (op) {
        case TOK_AMP:
            r.kind = ROP_PENTIT_AND;
            r.right_bp = PREC_BITWISE_AND;
            return r;
        case TOK_PIPE:
            r.kind = ROP_PENTIT_OR;
            r.right_bp = PREC_BITWISE_OR;
            return r;
        case TOK_STAR:
            r.kind = ROP_PENTIT_MUL;
            r.right_bp = PREC_MUL;
            return r;
        case TOK_STAR_STAR:
            r.kind = ROP_PENTIT_CONSENSUS;
            r.right_bp = PREC_MUL;
            return r;
        case TOK_PLUS:
            r.kind = ROP_PENTIT_ADD;
            r.right_bp = PREC_ADD;
            return r;
        case TOK_EQ_EQ:
            r.kind = ROP_PENTIT_EQ;
            r.right_bp = PREC_COMPARISON;
            r.result_type = type_bool();
            return r;
        case TOK_BANG_EQ:
            r.kind = ROP_PENTIT_NE;
            r.right_bp = PREC_COMPARISON;
            r.result_type = type_bool();
            return r;
        default:
            r.valid = false;
            r.error_msg = "operator not defined for pentit";
            return r;
        }
    }
```

Unary operators:
```c
    /* After the trit unary block */
    if (t->kind == TY_PENTIT && op == TOK_TILDE) {
        r.kind = ROP_PENTIT_INV;
        r.result_type = t;
        r.valid = true;
        return r;
    }
```

### 6. cg_type.c

**Location:** After TY_TRYTE (line 120-121)

```c
    case TY_PENTIT:
        return LLVMInt8TypeInContext(cg->ctx);   /* pentit: i8 {0,1,2,3,4} → {N2,N1,Z,P1,P2} */
    case TY_PENTYTE:
        return LLVMInt16TypeInContext(cg->ctx);   /* pentyte: 5 pentits packed in u16 */
```

Packed pentit arrays:
```c
    case TY_ARRAY:
        /* Packed trit arrays: 5 trits per byte */
        if (t->u.array.elem && type_resolve(t->u.array.elem)->kind == TY_TRIT) {
            uint64_t packed_bytes = (t->u.array.size + 4) / 5;
            return LLVMArrayType2(LLVMInt8TypeInContext(cg->ctx), packed_bytes);
        }
        /* Packed pentit arrays: 3 pentits per byte */
        if (t->u.array.elem && type_resolve(t->u.array.elem)->kind == TY_PENTIT) {
            uint64_t packed_bytes = (t->u.array.size + 2) / 3;
            return LLVMArrayType2(LLVMInt8TypeInContext(cg->ctx), packed_bytes);
        }
```

### 7. cg_expr.c

**Location:** After the trit operator codegen block (line 1704-1737)

Each operator from the truth tables above, implemented as LLVM IR. See the "Operator Truth Tables" section for exact codegen per operator.

### 8. cg_literal.c

**Location:** After the trit array literal packing (line 99-139)

```c
        /* Packed pentit array literal: pack elements 3-per-byte */
        if (elem_type->kind == TY_PENTIT) {
            uint64_t packed_bytes = (size + 2) / 3;
            LLVMTypeRef i8t = LLVMInt8TypeInContext(cg->ctx);
            LLVMTypeRef arr_t = LLVMArrayType2(i8t, packed_bytes);
            LLVMValueRef arr_alloca = cg_entry_alloca(cg, arr_t, "pentit.arr");

            /* Zero-initialize */
            for (uint64_t bi = 0; bi < packed_bytes; bi++) {
                LLVMValueRef bp = LLVMBuildGEP2(cg->builder, arr_t, arr_alloca,
                    (LLVMValueRef[]){
                        LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), 0, false),
                        LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), bi, false),
                    }, 2, "pentit.byte.init");
                LLVMBuildStore(cg->builder, LLVMConstInt(i8t, 0, false), bp);
            }

            /* Pack each pentit: 3 per byte using pow5 */
            static const uint8_t pow5[3] = {1, 5, 25};
            int i = 0;
            for (AstList *e = node->u.array_lit.elems; e; e = e->next, i++) {
                LLVMValueRef val = cg_expr(cg, e->node);
                uint64_t byte_idx = (uint64_t)i / 3;
                uint64_t pentit_pos = (uint64_t)i % 3;

                LLVMValueRef bp = LLVMBuildGEP2(cg->builder, arr_t, arr_alloca,
                    (LLVMValueRef[]){
                        LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), 0, false),
                        LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), byte_idx, false),
                    }, 2, "pentit.byte.ptr");
                LLVMValueRef cur = LLVMBuildLoad2(cg->builder, i8t, bp, "cur.byte");
                LLVMValueRef scaled = LLVMBuildMul(cg->builder, val,
                    LLVMConstInt(i8t, pow5[pentit_pos], false), "scaled");
                LLVMValueRef updated = LLVMBuildAdd(cg->builder, cur, scaled, "packed");
                LLVMBuildStore(cg->builder, updated, bp);
            }

            return LLVMBuildLoad2(cg->builder, arr_t, arr_alloca, "pentit.arr.val");
        }
```

Also add pentit array element extraction (index operator), mirroring the trit extraction at line ~973.

### 9. Pentit literal syntax

`pentit.N2`, `pentit.N1`, `pentit.Z`, `pentit.P1`, `pentit.P2`

Handled the same way as `trit.N`, `trit.Z`, `trit.P` — field access on the type name resolves to a constant.

In the codegen for field access on type identifiers:
```c
if (type_is_pentit && field_name_eq("N2")) return LLVMConstInt(i8t, 0, false);
if (type_is_pentit && field_name_eq("N1")) return LLVMConstInt(i8t, 1, false);
if (type_is_pentit && field_name_eq("Z"))  return LLVMConstInt(i8t, 2, false);
if (type_is_pentit && field_name_eq("P1")) return LLVMConstInt(i8t, 3, false);
if (type_is_pentit && field_name_eq("P2")) return LLVMConstInt(i8t, 4, false);
```

---

## Pentit ↔ Trit Conversion

Narrowing (pentit → trit): collapse intensity to direction.
```
pentit.N2 → trit.N    (strong negative → negative)
pentit.N1 → trit.N    (negative → negative)
pentit.Z  → trit.Z    (zero → zero)
pentit.P1 → trit.P    (positive → positive)
pentit.P2 → trit.P    (strong positive → positive)
```

In code: `trit_raw = clamp(pentit_raw, 0, 2)` or equivalently:
```c
/* pentit (0-4) to trit (0-2): divide by 2 */
trit_raw = pentit_raw / 2;  /* 0→0(N), 1→0(N), 2→1(Z), 3→1(Z), 4→2(P) */
```

Wait — that maps N1 to N and P1 to Z, which is wrong. Better:
```c
/* pentit to trit: sign-based */
if (pentit_raw < 2) return 0;       /* N2 or N1 → N */
if (pentit_raw == 2) return 1;      /* Z → Z */
return 2;                            /* P1 or P2 → P */
```

Widening (trit → pentit): preserve value, no intensity.
```
trit.N → pentit.N1    (negative → negative, not strong)
trit.Z → pentit.Z     (zero → zero)
trit.P → pentit.P1    (positive → positive, not strong)
```

In code: `pentit_raw = trit_raw + 1` — shifts from {0,1,2} to {1,2,3} which is {N1,Z,P1}.

Both conversions require explicit `as trit` or `as pentit` — no implicit narrowing or widening.

---

## JSF-Style Warning for 5-Variant Ordered Enums

In the checker, when processing an enum declaration:
```c
/* After parsing enum with all variants */
if (variant_count == 5) {
    /* Check if variants appear to be ordered with a center.
     * Heuristic: if variant names contain patterns like
     * negative/positive, low/high, strong/weak with a
     * middle/neutral/zero variant, suggest pentit. */
    check_warn(checker, enum_node->loc,
        "enum with 5 ordered variants — consider using 'pentit' "
        "if these represent balanced intensities");
}
```

This is a soft nudge, not a hard error. Same philosophy as the JSF float equality warning.

---

## Tests

### Test 1: Pentit literals and basic ops
```
fn Main() {
    a := pentit.P1;
    b := pentit.N1;
    Println(a & b);     // min(P1, N1) = N1 → prints 1
    Println(a | b);     // max(P1, N1) = P1 → prints 3
    Println(~a);        // ~P1 = N1 → prints 1
    Println(a ** b);    // consensus(P1, N1) = Z → prints 2
}
```

### Test 2: Pentit multiply with clamping
```
fn Main() {
    Println(pentit.P2 * pentit.P2);   // 2×2=4→clamp→P2 → prints 4
    Println(pentit.N2 * pentit.N2);   // −2×−2=4→clamp→P2 → prints 4
    Println(pentit.P2 * pentit.N1);   // 2×−1=−2=N2 → prints 0
    Println(pentit.P1 * pentit.Z);    // 1×0=0=Z → prints 2
}
```

### Test 3: Pentit add (mod-5)
```
fn Main() {
    Println(pentit.P2 + pentit.P1);   // 2+1=3→mod-5→N2 (wraps) → prints 0
    Println(pentit.P1 + pentit.P1);   // 1+1=2=P2 → prints 4
    Println(pentit.N1 + pentit.P1);   // −1+1=0=Z → prints 2
}
```

### Test 4: Packed pentit array
```
fn Main() {
    arr := [pentit.N2, pentit.Z, pentit.P2, pentit.N1, pentit.P1, pentit.Z];
    Println(arr[0]);  // N2 → 0
    Println(arr[2]);  // P2 → 4
    Println(arr[5]);  // Z → 2
}
```

### Test 5: Pentit ↔ trit conversion
```
fn Main() {
    p := pentit.P2;
    t := p as trit;
    Println(t);       // trit.P → 2

    t2 := trit.N;
    p2 := t2 as pentit;
    Println(p2);      // pentit.N1 → 1
}
```

### Test 6: Spaceship returning pentit
```
fn CompareIntensity(a i32, b i32) pentit {
    diff := a - b;
    if diff > 10 { return pentit.P2; }
    if diff > 0  { return pentit.P1; }
    if diff == 0 { return pentit.Z; }
    if diff > -10 { return pentit.N1; }
    return pentit.N2;
}

fn Main() {
    Println(CompareIntensity(100, 1));   // P2 → 4
    Println(CompareIntensity(5, 3));     // P1 → 3
    Println(CompareIntensity(5, 5));     // Z → 2
    Println(CompareIntensity(3, 5));     // N1 → 1
    Println(CompareIntensity(1, 100));   // N2 → 0
}
```

---

## Implementation Order

| Step | File | Change | Test |
|------|------|--------|------|
| 1 | types.h | Add TY_PENTIT, TY_PENTYTE | Compiles |
| 2 | token.h | Add TOK_KW_PENTIT, TOK_KW_PENTYTE | Compiles |
| 3 | lexer.c | Add keyword recognition | Compiles |
| 4 | cg_type.c | LLVM type lowering (i8, i16, packed arrays) | Compiles |
| 5 | cottrell.h | Add ROP_PENTIT_* enum values | Compiles |
| 6 | cottrell.c | Add pentit operator resolution | Compiles |
| 7 | cg_expr.c | Add pentit operator codegen (all 8 ops) | Test 1, 2, 3 pass |
| 8 | cg_literal.c | Pentit literal values + packed array packing | Test 4 passes |
| 9 | cg_literal.c | Pentit array extraction (index operator) | Test 4 passes |
| 10 | cg_expr.c | Pentit ↔ trit conversion codegen | Test 5 passes |
| 11 | checker | 5-variant enum warning | Warning fires on test enum |
| 12 | Full Furling Gate | **768/768 + 6 pentit tests** | All pass |

Each step builds on the previous. Each step tested. No stubs.
