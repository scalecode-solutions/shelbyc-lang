# Ternary Error System — Overview

**Date:** March 24, 2026
**C Bootstrap:** `/Users/travis/GitHub/ShelbyC/`
**LLVM:** 22.1.1

---

## What This Is

A unified error handling system with three exit paths, three defer triggers, one stack, one emit function.

| Value | Exit path | Defer trigger | Keyword | Meaning |
|-------|-----------|---------------|---------|---------|
| -1 | error | errdefer | `errdefer` | cleanup on failure |
| 0 | normal | defer | `defer` | cleanup always |
| +1 | recovered | recovery-defer | `recovery_defer` | cleanup after restart |

The `DeferEntry.trigger` and `exit_path` parameters are `int8_t` with three values (-1, 0, +1). This is genuinely ternary — there are exactly three exit paths and exactly three trigger types. Trit is the correct representation here.

---

## Where trit is correct vs where it isn't

**Genuinely ternary (use trit / int8_t with 3 values):**
- Defer trigger: errdefer(-1), defer(0), recovery(+1)
- Exit path: error(-1), normal(0), recovered(+1)
- trit/tryte scalar values and Cottrell operators
- Spaceship operator result: less(-1), equal(0), greater(+1)

**NOT ternary (use the type that fits):**
- `OwnershipState`: 5 values (ALIVE, BORROWED, CONSUMED, MAYBE_CONSUMED, DROPPED) — it's an enum, not a trit
- Flow effects: 6 kinds (READ, WRITE, CONSUME, BORROW, BORROW_MUT, DROP) — it's an enum
- `Result.is_ok`: 2 values — it's a bool
- `ND_DEFER.is_errdefer`: 2 values — it's a bool (the trigger trit is on DeferEntry, not on the AST)

Use the right type for the cardinality. Bool for 2. Trit for 3. Enum for more.

---

## Document Map

Each part is self-contained. Read any part independently. Every part includes the EXACT C bootstrap code (before and after) at the line level.

| Part | File | What it covers |
|------|------|---------------|
| 1 | [01-defer-stack-refactor.md](01-defer-stack-refactor.md) | Replace `AstNode **defers` with `DeferEntry *defers`. Zero behavior change. All 768 tests pass. |
| 2 | [02-exit-path-detection.md](02-exit-path-detection.md) | Map every exit point in the C bootstrap codegen. Assign exit path values. Handle the runtime-check case for `return result_variable`. |
| 3 | [03-errdefer.md](03-errdefer.md) | Add `errdefer` keyword, parser, codegen. Error path cleanup. Error capture with `\|e\|` syntax. |
| 4 | [04-conditions-restarts.md](04-conditions-restarts.md) | Add `restart`, `handle`, `invoke`, `on` keywords. Recovery path. Continuation-passing restart mechanism. |
| 5 | [05-flow-ownership-integration.md](05-flow-ownership-integration.md) | Connect to Flow Ownership. How each exit path affects ownership state. Coupled solver behavior at restart points. |
| 6 | [06-tensor-tile-connection.md](06-tensor-tile-connection.md) | TritTile type, scalar dot product, SIMD lowering (ARM NEON + x86 AVX2), ownership solver batch evaluation. |
| 7 | [07-implementation-order.md](07-implementation-order.md) | Step-by-step implementation plan. Each step tested before the next. Furling Gate at every milestone. |

---

## C Bootstrap Files Modified

| File | Lines | What changes |
|------|-------|-------------|
| `src/token.h` | 196 | Add `TOK_KW_ERRDEFER`, `TOK_KW_RESTART`, `TOK_KW_HANDLE`, `TOK_KW_INVOKE`, `TOK_KW_ON` |
| `src/lexer.c` | 845 | Add keyword recognition for new tokens |
| `src/ast.h` | 447 | Extend `ND_DEFER` union, add `ND_RESTART_DECL`, `ND_HANDLE_BLOCK`, `ND_HANDLE_ARM` |
| `src/parser.c` | 3400+ | Add errdefer parsing, restart declarations, handle blocks |
| `src/codegen.h` | 161 | Replace `AstNode **defers` with `DeferEntry *defers` in CgScope. Add `propagated_err_val` to Codegen. |
| `src/cg_internal.h` | 136 | Update function signatures for exit path parameter |
| `src/cg_stmt.c` | 1780 | Rewrite defer push/emit, update all cleanup call sites |
| `src/cg_literal.c` | 1633 | Update `?` propagation to pass error exit path, add restart codegen |
| `src/flow.h` | 259 | Add `defer_trigger` to FlowEffect struct |
| `src/flow.c` | 2130 | Path-aware ownership propagation at restart points |
| `src/runtime.c` | 1200+ | Add `__sc_trit_tile_dot` scalar + SIMD implementations |
| `src/types.h` | 321 | Add `TY_TRIT_TILE` type kind |
| `src/cg_type.c` | 437 | LLVM lowering for TritTile |
| `src/cg_builtin.c` | 637 | TritTile.Dot() codegen |

---

*Read the parts in order for the full design, or jump to any part for a specific implementation detail. Every part has the exact C bootstrap code with before/after diffs.*
