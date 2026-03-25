# Composable Error System — Overview

**Date:** March 25, 2026
**C Bootstrap:** `/Users/travis/GitHub/ShelbyC/`
**LLVM:** 22.1.1

---

## What This Is

A unified error handling system where the developer chooses the resolution that fits. The defer stack uses pentit-width triggers internally (i8, values 0-4), but the language surface exposes as much or as little as the code needs.

**The core principle: bit, trit, and pentit complement each other. Nothing replaces anything. Everything composes.**

---

## Three Levels of Resolution

### Bool level (2 paths) — what most code needs
```
defer { close(fd) }       // fires always
errdefer { rollback() }   // fires on error
```
Two keywords. Two paths. Covers 90% of error handling.

### Trit level (3 paths) — when you have conditions/restarts
```
defer { close(fd) }              // fires always
errdefer { rollback() }          // fires on error
recoverdefer { log("fixed") }   // fires on recovery
```
Three keywords. The recovery path from conditions/restarts. The trit system from the original design.

### Pentit level (5 paths) — full resolution
```
panicdefer { core_dump() }       // fires on panic only
errdefer { rollback() }          // fires on any error (including panic)
defer { close(fd) }              // fires always
successdefer { commit() }        // fires on any success (including recovery)
recoverdefer { log("fixed") }   // fires on recovery only
```
Five keywords. Panic isolation. Success-only cleanup. The full system.

---

## The Pentit Encoding

| Raw | Balanced | Exit path | Defer keyword | Fires on |
|-----|----------|-----------|---------------|----------|
| 0 | N2 (-2) | panic/fatal | `panicdefer` | {N2} only |
| 1 | N1 (-1) | error | `errdefer` | {N2, N1} — any negative |
| 2 | Z (0) | normal | `defer` | {N2, N1, Z, P1, P2} — always |
| 3 | P1 (+1) | success | `successdefer` | {Z, P1, P2} — any non-error |
| 4 | P2 (+2) | recovered | `recoverdefer` | {P2} only |

**The firing rule:** A defer fires if the exit is on its side and at least as extreme.
- `errdefer` (N1) catches N1 and the more extreme N2
- `successdefer` (P1) catches P1 and the more extreme P2
- `defer` (Z, center) catches everything
- `panicdefer` (N2) and `recoverdefer` (P2) are the extremes — most specific

The further from center, the more specific. The closer to center, the broader.

---

## Composability — Types Layer, Not Replace

A function returns with a trit: error, normal, recovered. The error handler on the N path might branch into a pentit for five levels of severity. One of those might branch into a bool — retry or abort.

```
trit: error / normal / recovered
       │
       └→ pentit: fatal / retriable / timeout / auth / validation
                    │
                    └→ bool: abort / retry
```

Each decision point uses the type that matches its branching factor. The defer stack is pentit-width so it can represent any of these. A developer using only `defer` and `errdefer` is at the bool level — the pentit infrastructure is invisible.

---

## Where Each Type is Correct

**Bool (2 values):**
- `Result.is_ok`: success or failure
- `ND_DEFER.is_errdefer`: normal defer or errdefer (parser representation)
- Most error handling code

**Trit (3 values):**
- Spaceship operator result: less / equal / greater
- Basic error system: error / normal / recovered
- trit/tryte scalar values and Cottrell operators

**Pentit (5 values):**
- Full defer trigger: panic / error / always / success / recovery
- OwnershipState: ALIVE / BORROWED / CONSUMED / MAYBE_CONSUMED / DROPPED
- pentit/pentyte scalar values and Cottrell operators

**Enum (more than 5):**
- Flow effects: 6 kinds (READ, WRITE, CONSUME, BORROW, BORROW_MUT, DROP)
- Token kinds, node kinds, type kinds

Use the right type for the cardinality. They compose. They don't replace each other.

---

## Document Map

Each part is self-contained. Read any part independently. Every part includes the EXACT C bootstrap code (before and after) at the line level.

| Part | File | What it covers |
|------|------|---------------|
| 1 | [01-defer-stack-refactor.md](01-defer-stack-refactor.md) | Replace `AstNode **defers` with `DeferEntry *defers`. Pentit-width trigger. Zero behavior change. |
| 2 | [02-exit-path-detection.md](02-exit-path-detection.md) | Map every exit point to a pentit value. Handle runtime detection for `return result_variable`. |
| 3 | [03-errdefer.md](03-errdefer.md) | Add `errdefer` keyword. N1 path. Error capture with `\|e\|` syntax. |
| 4 | [04-conditions-restarts.md](04-conditions-restarts.md) | Add `restart`, `handle`, `invoke`, `on`. P2 path. Continuation-passing restart mechanism. |
| 5 | [05-flow-ownership-integration.md](05-flow-ownership-integration.md) | Connect to Flow Ownership. Path-aware ownership propagation. Coupled solver with `on_path`. |
| 6 | [06-tensor-tile-connection.md](06-tensor-tile-connection.md) | TritTile type, scalar dot product, SIMD lowering. Separate from error system. |
| 7 | [07-implementation-order.md](07-implementation-order.md) | Step-by-step plan. Milestones. Furling Gate at every step. |

---

## C Bootstrap Files Modified

| File | What changes |
|------|-------------|
| `src/token.h` | Add `TOK_KW_ERRDEFER`, `TOK_KW_PANICDEFER`, `TOK_KW_SUCCESSDEFER`, `TOK_KW_RECOVERDEFER`, `TOK_KW_RESTART`, `TOK_KW_HANDLE`, `TOK_KW_INVOKE`, `TOK_KW_ON` |
| `src/lexer.c` | Keyword recognition for new tokens |
| `src/ast.h` | Extend `ND_DEFER` union, add `ND_RESTART_DECL`, `ND_HANDLE_BLOCK`, `ND_HANDLE_ARM` |
| `src/parser.c` | Parse all defer variants, restart declarations, handle blocks |
| `src/codegen.h` | `DeferEntry` with pentit trigger. `propagated_err_val` on Codegen. |
| `src/cg_internal.h` | Function signatures with exit path parameter |
| `src/cg_stmt.c` | Defer push/emit with pentit truth table, all cleanup call sites |
| `src/cg_literal.c` | `?` propagation with error exit path, restart codegen |
| `src/flow.h` | `defer_trigger` (pentit) on FlowEffect |
| `src/flow.c` | Path-aware ownership propagation, 5-path analysis |
| `src/types.h` | `TY_TRIT_TILE` type kind |
| `src/cg_type.c` | LLVM lowering for TritTile |
| `src/cg_builtin.c` | TritTile.Dot() codegen |

---

*Read the parts in order for the full design, or jump to any part for a specific implementation detail. Every part has the exact C bootstrap code with before/after diffs.*
