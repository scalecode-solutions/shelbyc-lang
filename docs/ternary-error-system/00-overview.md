# Ternary Error System — Overview

**Date:** March 24, 2026
**C Bootstrap:** `/Users/travis/GitHub/ShelbyC/`
**LLVM:** 22.1.1

---

## What This Is

A unified error handling system built on balanced ternary. Three exit paths, three defer triggers, one stack, one emit function.

| Trit | Exit path | Defer trigger | Keyword | Meaning |
|------|-----------|---------------|---------|---------|
| N (-1) | error | errdefer | `errdefer` | cleanup on failure |
| Z (0) | normal | defer | `defer` | cleanup always |
| P (+1) | recovered | recovery-defer | `recovery_defer` | cleanup after restart |

This is not three separate features bolted together. It's one ternary system — the same N/Z/P that drives trit values, Cottrell Confluence operators, and Flow Ownership states.

---

## Document Map

Each part is self-contained. Read any part independently. Every part includes the EXACT C bootstrap code (before and after) at the line level.

| Part | File | What it covers |
|------|------|---------------|
| 1 | [01-defer-stack-refactor.md](01-defer-stack-refactor.md) | Replace `AstNode **defers` with `DeferEntry *defers`. Zero behavior change. All 768 tests pass. |
| 2 | [02-exit-path-detection.md](02-exit-path-detection.md) | Map every exit point in the C bootstrap codegen. Assign trit values. Handle the runtime-check case for `return result_variable`. |
| 3 | [03-errdefer.md](03-errdefer.md) | Add `errdefer` keyword, parser, codegen. The N (-1) path. Error capture with `\|e\|` syntax. |
| 4 | [04-conditions-restarts.md](04-conditions-restarts.md) | Add `restart`, `handle`, `invoke`, `on` keywords. The P (+1) path. Continuation-passing restart mechanism. |
| 5 | [05-flow-ownership-integration.md](05-flow-ownership-integration.md) | Connect to Flow Ownership. OwnershipState mapping to trits. Coupled solver behavior on N/Z/P paths. Restart points as constraints. |
| 6 | [06-tensor-tile-connection.md](06-tensor-tile-connection.md) | Effects matrix as trit tile. Batch ownership propagation. Same hardware instruction for neural inference and safety analysis. |
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
| `src/cg_internal.h` | 136 | Update function signatures for trit exit path parameter |
| `src/cg_stmt.c` | 1780 | Rewrite defer push/emit, update all cleanup call sites |
| `src/cg_literal.c` | 1633 | Update `?` propagation to pass N exit path, add restart codegen |
| `src/flow.h` | 259 | Add `OS_BORROWED` to OwnershipState, update merge function |
| `src/flow.c` | 2130 | Trit-aware ownership merge, restart point handling |
| `src/cottrell.h` | 131 | No changes (trit ops already exist) |
| `src/cottrell.c` | 188 | No changes (trit ops already exist) |

---

## Ternary Coherence

Every level of ShelbyC uses the same three-valued logic:

```
trit scalar:      N(-1)      Z(0)       P(+1)
                  negative   zero       positive

Cottrell ops:     & (min)    + (mod-3)  | (max)
                  consensus  addition   dissensus

Ownership:        consumed   borrowed   alive
                  moved      lent       owned

Error path:       error      normal     recovered
                  unwind     succeed    restart

Defer trigger:    errdefer   defer      recovery_defer
                  N-only     N+Z        P-only

Typestate:        moved      borrowed   owned
                  dead       shared     exclusive

Tensor effect:    consume    unchanged  create
                  -1 select  0 skip     +1 add
```

One system. One mental model. Same trit at every scale.

---

*Read the parts in order for the full design, or jump to any part for a specific implementation detail. Every part has the exact C bootstrap code with before/after diffs.*
