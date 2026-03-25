# Part 7: Implementation Order

**Goal:** Step-by-step order for implementing the entire composable pentit-based error system in the C bootstrap. Each step is tested before the next. The Furling Gate runs at every milestone.

---

## Milestones

| Milestone | Steps | Tests | Description |
|-----------|-------|-------|-------------|
| **M1** | 1-5 | 772/772 | Refactor defer stack to DeferEntry with pentit trigger — zero behavior change |
| **M2** | 6-10 | 772 + 7 | Add errdefer + panicdefer keywords, parser, codegen — N-side paths functional |
| **M3** | 11-12 | 772 + 9 | Runtime path detection for `return result_variable` |
| **M4** | 13-21 | 772 + 16 | Conditions, restarts, successdefer, recoverdefer — P-side paths functional |
| **M5** | 22-24 | 772 + 22 | Flow Ownership integration — pentit-aware merge |
| **M6** | 25 | 772 + 22 | Full Furling Gate verification |

---

## Milestone 1: Defer stack refactor (Part 1)

Zero behavior change. Pure structural refactor.

| Step | File | Change | Test |
|------|------|--------|------|
| 1 | codegen.h | Add `DeferEntry` typedef with pentit trigger (i8, 0-4), change `AstNode **defers` to `DeferEntry *defers` | Compiles |
| 2 | codegen.h | Add `propagated_err_val` to Codegen struct | Compiles |
| 3 | cg_stmt.c | Replace `cg_push_defer` internals with DeferEntry, add `cg_push_panicdefer`, `cg_push_errdefer`, `cg_push_successdefer`, `cg_push_recoverdefer` | Compiles |
| 4 | cg_stmt.c | Add `cg_emit_scope_defers_pentit` with pentit firing rule truth table, old function becomes wrapper passing Z (2) | Compiles |
| 5 | cg_internal.h | Update function signatures — all `_pentit` suffixed functions | **772/772 Furling Gate** |

```bash
make clean && make -j8
cd furling-gate && SHELBYC_ROOT=/Users/travis/GitHub/ShelbyC cargo run --release --bin dial
# Expected: 772/772
```

---

## Milestone 2: Errdefer + Panicdefer — N-side paths (Parts 2-3)

| Step | File | Change | Test |
|------|------|--------|------|
| 6 | token.h | Add `TOK_KW_ERRDEFER`, `TOK_KW_PANICDEFER` | Compiles |
| 7 | lexer.c | Add `{"errdefer", TOK_KW_ERRDEFER}`, `{"panicdefer", TOK_KW_PANICDEFER}` to keyword table | Compiles |
| 8 | ast.h | Add `defer` union member with `trigger` (pentit i8), `err_capture` | Compiles |
| 9 | parser.c | Combine defer/errdefer/panicdefer parsing, add `\|e\|` capture | Parse test passes |
| 10 | cg_stmt.c | Update `ND_DEFER` case: trigger=0 -> `cg_push_panicdefer`, trigger=1 -> `cg_push_errdefer`, trigger=2 -> `cg_push_defer` | Compiles |

Now update exit paths (Part 2):

| Step | File | Change | Test |
|------|------|--------|------|
| 10a | cg_literal.c:915 | `?` error: set `propagated_err_val`, call `_pentit(cg, 1)` (N1) | |
| 10b | cg_literal.c:919 | `?` void: call `_pentit(cg, 1)` (N1) | |
| 10c | cg_stmt.c:345 | return: detect `ND_ERR` -> N1 (1), `ND_OK` -> Z (2), else -> Z (2) | |
| 10d | cg_stmt.c:316 | TCO: `_pentit(cg, 2)` (Z) | |
| 10e | panic builtin | panic: `_pentit(cg, 0)` (N2) | **772/772 + 7 errdefer/panicdefer tests** |

```bash
make clean && make -j8
cd furling-gate && SHELBYC_ROOT=/Users/travis/GitHub/ShelbyC cargo run --release --bin dial
# Run errdefer tests: basic, ?, capture, mixed ordering, no-fire-on-success, nested, panicdefer-only
# Expected: 772/772 + 7/7
```

---

## Milestone 3: Runtime path detection (Part 2 completion)

| Step | File | Change | Test |
|------|------|--------|------|
| 11 | cg_stmt.c | Add `cg_emit_result_path_branch` function (branches on is_ok, calls `_pentit` with 2 for ok, 1 for err) | Compiles |
| 12 | cg_stmt.c:345 | `return result_var` -> call `cg_emit_result_path_branch` | **772/772 + 9 tests** |

New tests:
- Return ok variable -> Z path (errdefer doesn't run)
- Return err variable -> N1 path (errdefer runs)

```bash
make clean && make -j8
# Run all tests
# Expected: 772/772 + 9/9
```

---

## Milestone 4: Conditions, restarts, successdefer, recoverdefer — P-side paths (Part 4)

| Step | File | Change | Test |
|------|------|--------|------|
| 13 | token.h | Add `TOK_KW_RESTART`, `TOK_KW_HANDLE`, `TOK_KW_INVOKE`, `TOK_KW_ON`, `TOK_KW_SUCCESSDEFER`, `TOK_KW_RECOVERDEFER` | Compiles |
| 14 | lexer.c | Add 6 keywords to table | Compiles |
| 15 | ast.h | Add `ND_RESTARTABLE`, `ND_RESTART_DECL`, `ND_HANDLE_BLOCK`, `ND_HANDLE_ARM` with union members | Compiles |
| 16 | parser.c | Extend defer parsing for successdefer (trigger=3) and recoverdefer (trigger=4) | Parse test |
| 17 | parser.c | Parse restart declarations after expressions | Parse test |
| 18 | parser.c | Parse handle blocks | Parse test |
| 19 | cg_stmt.c | Update `ND_DEFER` case for trigger=3 (`cg_push_successdefer`) and trigger=4 (`cg_push_recoverdefer`) | Compiles |
| 20 | cg_literal.c | Codegen for `ND_RESTARTABLE`: switch on restart choice, emit restart bodies, PHI merge. Recovery exit uses `_pentit(cg, scope, 4)` for P2 | |
| 21 | cg_expr.c | Codegen for `ND_HANDLE_BLOCK`: set up restart choice alloca | **772/772 + 16 tests** |

Also:
| Step | File | Change |
|------|------|--------|
| 21a | codegen.h | Add `restart_choice_alloca` to Codegen struct |

New tests:
- Basic restart with handle
- Restart preserves resources (errdefer not fired)
- No handle -> normal error propagation
- Multiple restarts, caller picks
- Restart with parameters
- Nested restarts
- recoverdefer fires on recovery, not on error or normal
- successdefer fires on P1/P2, not on N1/N2

```bash
make clean && make -j8
# Run all tests
# Expected: 772/772 + 16/16
```

---

## Milestone 5: Flow Ownership integration (Part 5)

| Step | File | Change | Test |
|------|------|--------|------|
| 22 | flow.h | Add `OS_BORROWED` to OwnershipState enum, add `defer_trigger` (pentit i8) to FlowEffect | Compiles |
| 23 | flow.c | Update `ownership_merge` with 5-state logic | Compiles |
| 24 | flow.c | Pentit-aware ownership propagation: filter effects by pentit firing rule, run up to 5 passes (one per relevant path) | **772/772 + 22 tests** |

New tests:
- Flow analysis: errdefer consumes variable on N1 path
- Flow analysis: panicdefer consumes variable on N2 path
- Flow analysis: successdefer only fires on P1/P2 paths
- Flow analysis: restart preserves variable on P2 path
- Flow analysis: borrow in restart body doesn't outlive restart scope
- Flow analysis: mixed defer/errdefer/panicdefer/successdefer/recoverdefer ownership correctness

```bash
make clean && make -j8
cd furling-gate && SHELBYC_ROOT=/Users/travis/GitHub/ShelbyC cargo run --release --bin dial
# Expected: 772/772 + 22/22
```

---

## Milestone 6: Full verification

| Step | Description |
|------|-------------|
| 25 | Run the COMPLETE Furling Gate + all new tests. Every existing test passes. Every new test passes. No warnings. No regressions. |

```bash
cd /Users/travis/GitHub/ShelbyC
make clean && make -j8
cd furling-gate && SHELBYC_ROOT=/Users/travis/GitHub/ShelbyC cargo run --release --bin dial
# Expected: 772/772 + 22/22 = 794/794
```

---

## After implementation

The composable pentit-based error system is proven in the C bootstrap. All five paths (N2/N1/Z/P1/P2) are tested. Flow Ownership verifies safety on each path.

The ShelbyC rewrite inherits this system by carrying it over from the C bootstrap's implementation:
- **token.h** keywords -> ShelbyC TokenKind enum
- **lexer.c** keyword table -> ShelbyC lexer KW function
- **ast.h** nodes -> ShelbyC NodeKind enum + Node struct fields
- **parser.c** parsing -> ShelbyC parser functions
- **cg_stmt.c** + **cg_literal.c** codegen -> ShelbyC codegen functions (all `_pentit` suffixed)
- **flow.h** + **flow.c** ownership -> ShelbyC flow analysis (pentit-aware propagation)

Every function in the self-hosted compiler has a corresponding function in the C bootstrap. The C bootstrap is the source of truth.

---

## Keyword summary

| Keyword | Pentit trigger | Raw value | Added in milestone |
|---------|---------------|-----------|-------------------|
| `defer` | Z | 2 | Existing (M1 refactors internals) |
| `errdefer` | N1 | 1 | M2 |
| `panicdefer` | N2 | 0 | M2 |
| `successdefer` | P1 | 3 | M4 |
| `recoverdefer` | P2 | 4 | M4 |
| `restart` | (mechanism) | — | M4 |
| `handle` | (mechanism) | — | M4 |
| `invoke` | (mechanism) | — | M4 |
| `on` | (mechanism) | — | M4 |

---

## Timeline estimate

| Milestone | Effort |
|-----------|--------|
| M1: Refactor | 1-2 hours (mechanical, zero risk) |
| M2: Errdefer + Panicdefer | 3-4 hours (parser + codegen + tests) |
| M3: Runtime detect | 1 hour (one function + 2 tests) |
| M4: Restarts + Successdefer + Recoverdefer | 8-10 hours (most complex part — parser + codegen + handle mechanism + 2 new defer types) |
| M5: Flow integration | 3-4 hours (merge function + 5-path handling) |
| M6: Verification | 1 hour (run everything, fix any issues) |

**Total: ~17-22 hours of implementation, spread across sessions.**

Each milestone is independently valuable:
- After M2: errdefer + panicdefer are usable (biggest practical win, bool level)
- After M4: full pentit system including successdefer + recoverdefer (unique to ShelbyC)
- After M5: safety-verified pentit error handling (no other language has this)
