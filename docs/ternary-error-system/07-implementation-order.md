# Part 7: Implementation Order

**Goal:** Step-by-step order for implementing the entire ternary error system in the C bootstrap. Each step is tested before the next. The Furling Gate runs at every milestone.

---

## Milestones

| Milestone | Steps | Tests | Description |
|-----------|-------|-------|-------------|
| **M1** | 1-5 | 768/768 | Refactor defer stack to DeferEntry — zero behavior change |
| **M2** | 6-10 | 768 + 6 | Add errdefer keyword, parser, codegen — N path functional |
| **M3** | 11-12 | 768 + 8 | Runtime path detection for `return result_variable` |
| **M4** | 13-19 | 768 + 14 | Conditions and restarts — P path functional |
| **M5** | 20-22 | 768 + 18 | Flow Ownership integration — trit-aware merge |
| **M6** | 23 | 768 + 18 | Full Furling Gate verification |

---

## Milestone 1: Defer stack refactor (Part 1)

Zero behavior change. Pure structural refactor.

| Step | File | Change | Test |
|------|------|--------|------|
| 1 | codegen.h | Add `DeferEntry` typedef, change `AstNode **defers` to `DeferEntry *defers` | Compiles |
| 2 | codegen.h | Add `propagated_err_val` to Codegen struct | Compiles |
| 3 | cg_stmt.c | Replace `cg_push_defer` internals with DeferEntry, add `cg_push_errdefer`, `cg_push_recovery_defer` | Compiles |
| 4 | cg_stmt.c | Add `cg_emit_scope_defers_trit` with truth table, old function becomes wrapper | Compiles |
| 5 | cg_internal.h | Update function signatures | **768/768 Furling Gate** |

```bash
make clean && make -j8
cd furling-gate && SHELBYC_ROOT=/Users/travis/GitHub/ShelbyC cargo run --release --bin dial
# Expected: 768/768
```

---

## Milestone 2: Errdefer — N path (Parts 2-3)

| Step | File | Change | Test |
|------|------|--------|------|
| 6 | token.h | Add `TOK_KW_ERRDEFER` | Compiles |
| 7 | lexer.c | Add `{"errdefer", TOK_KW_ERRDEFER}` to keyword table | Compiles |
| 8 | ast.h | Add `defer` union member with `is_errdefer`, `err_capture` | Compiles |
| 9 | parser.c | Combine defer/errdefer parsing, add `\|e\|` capture | Parse test passes |
| 10 | cg_stmt.c | Update `ND_DEFER` case to call `cg_push_errdefer` when `is_errdefer` | Compiles |

Now update exit paths (Part 2):

| Step | File | Change | Test |
|------|------|--------|------|
| 10a | cg_literal.c:915 | `?` error: set `propagated_err_val`, call `_trit(cg, -1)` | |
| 10b | cg_literal.c:919 | `?` void: call `_trit(cg, -1)` | |
| 10c | cg_stmt.c:345 | return: detect `ND_ERR` → N, `ND_OK` → Z, else → Z | |
| 10d | cg_stmt.c:316 | TCO: `_trit(cg, 0)` | **768/768 + 6 errdefer tests** |

```bash
make clean && make -j8
cd furling-gate && SHELBYC_ROOT=/Users/travis/GitHub/ShelbyC cargo run --release --bin dial
# Run errdefer tests: basic, ?, capture, mixed ordering, no-fire-on-success, nested
# Expected: 768/768 + 6/6
```

---

## Milestone 3: Runtime path detection (Part 2 completion)

| Step | File | Change | Test |
|------|------|--------|------|
| 11 | cg_stmt.c | Add `cg_emit_result_path_branch` function | Compiles |
| 12 | cg_stmt.c:345 | `return result_var` → call `cg_emit_result_path_branch` | **768/768 + 8 tests** |

New tests:
- Return ok variable → Z path (errdefer doesn't run)
- Return err variable → N path (errdefer runs)

```bash
make clean && make -j8
# Run all tests
# Expected: 768/768 + 8/8
```

---

## Milestone 4: Conditions and restarts — P path (Part 4)

| Step | File | Change | Test |
|------|------|--------|------|
| 13 | token.h | Add `TOK_KW_RESTART`, `TOK_KW_HANDLE`, `TOK_KW_INVOKE`, `TOK_KW_ON` | Compiles |
| 14 | lexer.c | Add 4 keywords to table | Compiles |
| 15 | ast.h | Add `ND_RESTARTABLE`, `ND_RESTART_DECL`, `ND_HANDLE_BLOCK`, `ND_HANDLE_ARM` with union members | Compiles |
| 16 | parser.c | Parse restart declarations after expressions | Parse test |
| 17 | parser.c | Parse handle blocks | Parse test |
| 18 | cg_literal.c | Codegen for `ND_RESTARTABLE`: switch on restart choice, emit restart bodies, PHI merge | |
| 19 | cg_expr.c | Codegen for `ND_HANDLE_BLOCK`: set up restart choice alloca | **768/768 + 14 tests** |

Also:
| Step | File | Change |
|------|------|--------|
| 19a | codegen.h | Add `restart_choice_alloca` to Codegen struct |

New tests:
- Basic restart with handle
- Restart preserves resources (errdefer not fired)
- No handle → normal error propagation
- Multiple restarts, caller picks
- Restart with parameters
- Nested restarts

```bash
make clean && make -j8
# Run all tests
# Expected: 768/768 + 14/14
```

---

## Milestone 5: Flow Ownership integration (Part 5)

| Step | File | Change | Test |
|------|------|--------|------|
| 20 | flow.h | Add `OS_BORROWED` to OwnershipState enum | Compiles |
| 21 | flow.c | Update `ownership_merge` with trit-aware logic | Compiles |
| 22 | flow.c | Verify ownership propagation handles N/Z/P paths at restart points | **768/768 + 18 tests** |

New tests:
- Flow analysis: errdefer consumes variable on N path
- Flow analysis: restart preserves variable on P path
- Flow analysis: borrow in restart body doesn't outlive restart scope
- Flow analysis: mixed defer/errdefer/restart ownership correctness

```bash
make clean && make -j8
cd furling-gate && SHELBYC_ROOT=/Users/travis/GitHub/ShelbyC cargo run --release --bin dial
# Expected: 768/768 + 18/18
```

---

## Milestone 6: Full verification

| Step | Description |
|------|-------------|
| 23 | Run the COMPLETE Furling Gate + all new tests. Every existing test passes. Every new test passes. No warnings. No regressions. |

```bash
cd /Users/travis/GitHub/ShelbyC
make clean && make -j8
cd furling-gate && SHELBYC_ROOT=/Users/travis/GitHub/ShelbyC cargo run --release --bin dial
# Expected: 768/768 + 18/18 = 786/786
```

---

## After implementation

The ternary error system is proven in the C bootstrap. All three paths (N/Z/P) are tested. Flow Ownership verifies safety on each path.

The ShelbyC rewrite inherits this system by carrying it over from the C bootstrap's implementation:
- **token.h** keywords → ShelbyC TokenKind enum
- **lexer.c** keyword table → ShelbyC lexer KW function
- **ast.h** nodes → ShelbyC NodeKind enum + Node struct fields
- **parser.c** parsing → ShelbyC parser functions
- **cg_stmt.c** + **cg_literal.c** codegen → ShelbyC codegen functions
- **flow.h** + **flow.c** ownership → ShelbyC flow analysis

Every function in the self-hosted compiler has a corresponding function in the C bootstrap. The C bootstrap is the source of truth.

---

## Timeline estimate

| Milestone | Effort |
|-----------|--------|
| M1: Refactor | 1-2 hours (mechanical, zero risk) |
| M2: Errdefer | 3-4 hours (parser + codegen + tests) |
| M3: Runtime detect | 1 hour (one function + 2 tests) |
| M4: Restarts | 6-8 hours (most complex part — parser + codegen + handle mechanism) |
| M5: Flow integration | 2-3 hours (merge function + path handling) |
| M6: Verification | 1 hour (run everything, fix any issues) |

**Total: ~14-18 hours of implementation, spread across sessions.**

Each milestone is independently valuable:
- After M2: errdefer is usable (biggest practical win)
- After M4: full ternary system (unique to ShelbyC)
- After M5: safety-verified ternary error handling (no other language has this)
