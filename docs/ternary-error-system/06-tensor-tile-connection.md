# Part 6: Tensor Tile Connection

**Goal:** Show how the ternary defer system's ownership effects map to the tensor tile infrastructure. The effects matrix for a function IS a trit tile. Batch ownership propagation uses the same hardware instruction as neural inference.

**Files modified:** None (this part is architectural — it describes how the existing tensor tile design from the blog post connects to the error system)

**Depends on:** Parts 1-5, tensor-type-system.md blog post, self-hosting-semantics.md blog post

---

## The effects matrix

A function has N variables and K effects (reads, writes, borrows, consumes, drops). Each effect changes one variable's ownership state. This is a matrix:

```
                 Effect 0   Effect 1   Effect 2   Effect 3   Effect 4
                 (Alloc)    (ReadFile) (errdefer) (defer)    (?error)
Variable buf:    P(+1)      Z(0)       N(-1)      Z(0)       Z(0)
Variable data:   Z(0)       P(+1)      Z(0)       Z(0)       N(-1)
Variable conn:   Z(0)       Z(0)       Z(0)       Z(0)       Z(0)
```

Each cell is a trit:
- **P (+1):** this effect creates/revives the variable (WRITE)
- **Z (0):** this effect doesn't change the variable (READ or no-op)
- **N (-1):** this effect consumes/destroys the variable (CONSUME/DROP)

---

## The connection to TritTile

From `tensor-type-system.md`:

```
primitive TritTile<M, K> {
    weights [M][K]trit;
    input   [K]i8;
}

fn (t &TritTile<M, K>) Infer() [M]i32 {
    return t.weights.Dot(t.input);
}
```

The ownership effects matrix is a `TritTile<N, K>`:
- **weights** = `[N][K]trit` — N variables × K effects, each cell is a trit
- **input** = `[K]i8` — which effects are active on the current path (1=active, 0=inactive)
- **output** = `[N]i32` — new ownership state per variable (accumulated trit changes)

For the **Z path** (normal): all defers are active, errdefers are not:
```
input = [1, 1, 0, 1, 0]   // effects 0,1,3 active; errdefer(2) and ?error(4) inactive
```

For the **N path** (error): defers AND errdefers are active:
```
input = [1, 1, 1, 1, 1]   // all effects active
```

For the **P path** (recovery): only recovery-specific effects:
```
input = [1, 1, 0, 0, 0]   // only creation effects active; cleanup not needed
```

The `TritTile.Infer()` operation applies the effects to produce the new ownership state vector. The compiler lowers this to:
- **ARM NEON:** `TBL` (table lookup) + `SADDLV` (horizontal add)
- **x86 AVX2:** `VPSHUFB` (shuffle) + `VPADDD` (packed add)

**The same SIMD instruction** that runs ternary neural inference runs ownership analysis. The developer's AI code and the compiler's safety checker share hardware.

---

## Batch propagation

From `self-hosting-semantics.md`:

```
fn BatchPropagate<N, K>(
    states    [N]trit,
    effects   TritTile<N, K>,
) [N]trit {
    return effects.Infer(states);
}
```

The ownership solver calls this for each basic block:

```
fn PropagateBlock(states [N]trit, block &FlowBlock, exit_path trit) [N]trit {
    // Filter effects by exit path
    active := FilterEffects(block.effects, exit_path);

    // Build trit tile from active effects
    tile := TritTile<N, K>{ weights: active.matrix, input: active.mask };

    // Batch propagate — one SIMD operation
    return tile.Infer(states);
}
```

For a function with 100 variables and 50 effects per block:
- **Without batch:** 50 sequential switch-case evaluations
- **With batch:** 1 trit tile operation (vectorized to SIMD width)

The speedup scales with variable count. For the compiler compiling itself (hundreds of variables per function), this is significant.

---

## Separable decomposition

From `tensor-type-system.md`, the separable decomposition insight:

The effects matrix is sparse — most effects touch only one variable. This means it's separable:

- **Depthwise pass (L1):** per-variable analysis. Each variable's effects are independent. O(N).
- **Pointwise pass (L2):** cross-variable interactions. Only borrows that connect two variables need joint analysis. O(interactions).

The ownership solver from the coupled-ownership-solver blog IS a separable decomposition:
- **L1 (fast loop):** depthwise — each variable analyzed independently
- **L2+ (slow loop):** pointwise — only interactions (borrows, captures) need cross-variable analysis

The factored analysis matches separable convolutions: most of the work is in independent dimensions. The coupled solver is faster than a monolithic solver for the same reason depthwise-separable convolutions are faster than standard convolutions.

---

## The full stack

```
Developer code
  → uses trit arithmetic, closures, wakes, defer/errdefer/restart
  ↓ compiled by
Ownership solver
  → propagates [N]trit state vectors through CFG
  → effects matrix is a TritTile
  → batch evaluation via SIMD
  ↓ verified by
Flow Ownership (L1-L6)
  → coupled loops: fast (depthwise), slow (pointwise)
  → failure propagation as structured constraints
  → ternary exit paths (N/Z/P) determine which effects are active
  ↓ compiled to
LLVM IR
  → TBL + SADDLV on ARM NEON
  → VPSHUFB + VPADDD on x86 AVX2
  → same instructions for neural inference AND ownership analysis
```

The developer writes `errdefer Free(buf)`. The compiler represents this as a trit (-1) in the effects matrix. The SIMD instruction that evaluates whether Free(buf) should run is the same instruction that evaluates whether a ternary neural network weight adds, subtracts, or skips an activation.

One language. One trit. One instruction. Used for error handling, memory safety, and AI inference.

---

## Implementation note

The tensor tile infrastructure requires:
1. **Comptime evaluation** — `comptime fn TritLUT<Width>()`
2. **Fixed-size arrays** — `[N]trit` with comptime N
3. **SIMD intrinsics** — target-specific lowering

These are Phase 8 (comptime), Phase 1.6 (arrays), and future (SIMD). The ternary defer system works WITHOUT tensor tiles — the sequential evaluation in `cg_emit_scope_defers_trit` is functionally equivalent. The tensor tile connection is an optimization that makes the sequential evaluation parallelizable once the infrastructure exists.

The design is ready for it. The DeferEntry trigger is a trit. The effects matrix is a trit matrix. When tensor tiles are available, the solver plugs in directly.
