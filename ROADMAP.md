# ShelbyC Roadmap — Ground-Up Rewrite

**Started:** March 24, 2026
**Repo:** `shelbyc-lang` (clean start)
**Proto:** `shelbyc-lang-proto` (lessons learned, 22 bootstrap bugs)
**C Bootstrap:** `/Users/travis/GitHub/ShelbyC/` (source of truth)

> *"No shortcuts. If it's in the spec, prove it in C first."*
> The prototype taught us what NOT to do. This time we build it right.

---

## Architecture Rules (Non-Negotiable)

These are in the foundation. Not added later. Not "simplified for now."

1. **Multi-file from day one.** `shelbyc.mod` with `src` entries. No monolith.
2. **Error reporting from the first commit.** Every error path prints `file:line:col: error: message`. No silent failures. No error counts without descriptions.
3. **Type system matches the C bootstrap.** `TyTypeParam` entries with indices. `TypeSubstitute` handles ALL type kinds including TyStruct. `MonoEntry` stores `fn_type_idx` (substituted function type).
4. **Codegen resolves through the checker's Type system.** `ResolveTypeNode2(checker_type_idx)`, never AST node string matching. Field types resolved through `cg_llvm_type` equivalent, not `LLVMStructGetTypeAtIndex`.
5. **Anonymous LLVM types for generic structs.** `LLVMStructTypeInContext` not `LLVMStructCreateNamed`. No name collisions between `Result<i32,i32>` and `Result<HttpResponse,i32>`.
6. **CStr uses global string constants.** `LLVMBuildGlobalStringPtr` for string literals. No stack pointer lifetime issues. No `MakeCStr` workarounds.
7. **Short-circuit `&&` / `||`.** Compiled as conditional branches, not `LLVMBuildAnd`/`LLVMBuildOr`. No more nested `if` workarounds for null checks.
8. **LLVM 22 from the start.** Link against `libLLVM-22`. Opaque pointers only. No deprecated API.
9. **Codegen reports unhandled nodes.** `EmitExpr` and `EmitStmt` print diagnostics for unrecognized node kinds. No silent `return 0`.
10. **Every feature proved in C bootstrap first.** Nothing goes into shelbyc that hasn't been tested in the C bootstrap.

---

## What We Carry Over (Works, Just Clean Up)

These are correct in the prototype. Copy, clean up formatting, add error reporting.

| Component | Source | Lines | Notes |
|-----------|--------|-------|-------|
| Lexer | `lexer.smc` | ~500 | Complete, correct. Add file tracking for multi-file errors. |
| Parser | `parser.smc` | ~1900 | Complete with speculative generic parsing. Clean up error messages. |
| Checker (structure) | `checker.smc` | ~2000 | Pass 1/2/3 structure, Unify, symbol table, scope management. Rewrite type param and mono handling. |
| Flow framework | `flow.smc` | ~800 | FlowGraph/Block/Var/Effect structure. Not enforced yet. |
| LLVM FFI | `llvm_ffi.smc` | ~200 | All extern declarations. Update any deprecated calls. |
| CLI structure | `main.smc` | ~500 | build, run, -o, shelbyc.mod, ReadModSources, linking. |
| Runtime | `runtime/` | ~1200 | runtime.c, runtime_sched.c, runtime_asm_arm64.s. No changes. |

---

## What We Rewrite (Using C Bootstrap as Reference)

These are broken or shortcutted in the prototype. Rewrite from scratch, function by function, comparing against the C bootstrap's implementation.

### Checker Rewrites

| Function | C Bootstrap Location | What Changes |
|----------|---------------------|-------------|
| Type param registration | `checker.c` — `register_type_params()` | Create `TyTypeParam` entries with indices during Pass 2 |
| `TypeSubstitute` | `types.c` — `checker_substitute_type()` | Handle TyStruct (kind 23): substitute inner/ret_type. Handle ALL compound types. |
| `RecordMono` / `RecordMono2` | `flow_analysis.c` — mono entry creation | Store `fn_type_idx`: call TypeSubstitute on the function type to get fully-resolved concrete type |
| `MonoEntry` struct | `flow_analysis.h` | Add `fn_type_idx i32` — substituted function type index |

### Codegen Rewrites

| Function | C Bootstrap Location | What Changes |
|----------|---------------------|-------------|
| `ForwardDeclareMonos` | `cg_decl.c:705-746` | Resolve return type via `ResolveTypeNode2(me.fn_type_idx.ret_type)` not `ResolveReturnType(ast_node)` |
| `ResolveTypeNode2` k==23 | `cg_type.c` — `cg_llvm_type` TY_STRUCT | Anonymous types for generics. Field substitution via `st.inner`/`st.ret_type` with mono context fallback. |
| `ResolveFieldLLVMType` | `cg_expr.c` — `cg_llvm_type(field_type)` | Resolve through checker Type, never `LLVMStructGetTypeAtIndex` |
| `EmitExpr` FieldAccess | `cg_expr.c:2113-2213` | Use checker type for field load type, not LLVM struct query |
| `EmitExpr` StructLit | `cg_literal.c` | Anonymous types, field type from checker |
| `EmitExpr` ? operator | `cg_expr.c` | Field types from checker via `InferTypeFromExpr` |
| `EmitExpr` match | `cg_stmt.c` | Scrutinee field types from checker |
| `EmitExpr` Binary `&&`/`||` | `cg_expr.c` | Short-circuit branches: `if lhs { rhs } else { false }` |
| CStr built-in | `cg_builtin.c` — `cg_sso_ptr` | `LLVMBuildGlobalStringPtr` for literals, `__sc_sso_ptr` for variables |

---

## Build Order

Each step compiled and tested by the C bootstrap before moving to the next. No step depends on unproven code.

### Phase 0: Foundation
- [ ] `shelbyc.mod` with src entries
- [ ] `types.smc` — all shared enums, structs, essential externs
- [ ] Error reporting infrastructure (`ParseError`, `CheckError` with file:line:col)
- [ ] Build with C bootstrap, verify empty program compiles

### Phase 1: Lexer + Parser
- [ ] `lexer.smc` — carry over from proto, add file index tracking
- [ ] `parser.smc` — carry over from proto with speculative generic parsing
- [ ] Test: tokenize and parse a ShelbyC file, verify node count

### Phase 2: Checker
- [ ] `checker.smc` — Pass 1 (collect declarations), Pass 2 (resolve types), Pass 3 (check bodies)
- [ ] Type param registration: create `TyTypeParam` entries
- [ ] `TypeSubstitute`: handle ALL type kinds including TyStruct
- [ ] `RecordMono` with `fn_type_idx`
- [ ] Test: type check `fn Add(a i32, b i32) i32`, verify types

### Phase 3: Hello World Codegen
- [ ] `llvm_ffi.smc` — LLVM 22 extern declarations
- [ ] `codegen.smc` — `NewCodegen`, `EmitAll`, `DeclareRuntime`, `EmitMainWrapper`
- [ ] `EmitFn`, `EmitBlock`, `EmitStmt` (VarDecl, Return, ExprStmt, Assign)
- [ ] `EmitExpr` (IntLit, StrLit, BoolLit, Ident, Binary, Unary, Call)
- [ ] CStr via `LLVMBuildGlobalStringPtr`
- [ ] `main.smc` — CLI, Compile, WriteObject, LinkWithFlags
- [ ] Test: `fn Main() { Println(42); }` → outputs 42

### Phase 4: Control Flow
- [ ] If/else
- [ ] While loops
- [ ] For loops
- [ ] Break/continue
- [ ] Short-circuit `&&` / `||`
- [ ] Test: FizzBuzz

### Phase 5: Structs + Methods
- [ ] Struct declarations, struct literals
- [ ] Field access (read + write)
- [ ] Methods with receivers
- [ ] ResolveFieldLLVMType through checker (not LLVMStructGetTypeAtIndex)
- [ ] Test: `struct Point { x i32; y i32; }` with methods

### Phase 6: Closures
- [ ] No-capture closures
- [ ] Closures with captures (env struct)
- [ ] Passing closures to higher-order functions
- [ ] Test: `GMap(items, fn(x i32) i32 { return x + offset; })`

### Phase 7: Generics
- [ ] Generic function declarations and monomorphization
- [ ] `ForwardDeclareMonos` with `fn_type_idx` — resolve through Type system
- [ ] Multi-type parameter generics (`<T, U>`)
- [ ] Per-call-site dispatch (CallSite table)
- [ ] Test: `fn Max<T>(a T, b T) T` with i32 and i64

### Phase 8: Generic Structs (The Hard Part — Done Right)
- [ ] `struct Option<T>` — anonymous LLVM types, field substitution
- [ ] `struct Result<T, E>` — two-param substitution
- [ ] `fn Some<T>(val T) Option<T>` — generic fn returning generic struct
- [ ] Multiple instantiations in same program (`Result<i32,i32>` + `Result<i64,i32>`)
- [ ] Test: full Option/Result with Some, Ok, Err, field access through Result.ok_val

### Phase 9: Interfaces
- [ ] Interface declarations
- [ ] Satisfaction checking (implicit)
- [ ] Dynamic dispatch via vtable (fat pointers)
- [ ] Test: `interface Describable { fn Describe() str; }`

### Phase 10: Pattern Matching + Error Propagation
- [ ] `match` with literal, wildcard, destructuring patterns
- [ ] `ok(val)`, `err(e)`, `some(val)` patterns
- [ ] Exhaustiveness checking (JSF Rule 3)
- [ ] `?` operator (check is_ok, early-return, unwrap)
- [ ] Must-use Result (JSF Rule 115)
- [ ] Test: scJSON-style match destructuring

### Phase 11: Flow Ownership
- [ ] L1: Static flow (typestate propagation, move tracking, drop insertion)
- [ ] L2: Region inference (scoped borrows)
- [ ] Float equality warning (JSF Rule 202)
- [ ] Signed/unsigned mixing warning (JSF Rule 162)
- [ ] Loop counter mutation warning (JSF Rule 201)
- [ ] Test: use-after-move detection, borrow lifetimes

### Phase 12: Multi-File + CLI
- [ ] `ReadModSources` with proper `Vec<str>` path handling
- [ ] Source concatenation without extra newlines
- [ ] File index tracking for error reporting across files
- [ ] build, run, -o flags
- [ ] Link with shelbyc.mod link_path/link_lib
- [ ] Test: split compiler compiles multi-file test program

### Phase 13: Self-Compilation
- [ ] Compile the split source with C bootstrap → shelbyc1
- [ ] shelbyc1 compiles the split source → shelbyc2
- [ ] shelbyc2 compiles the split source → shelbyc3
- [ ] shelbyc3 runs all tests
- [ ] **Triple Test: PASS**

---

## New Features (Prove in C Bootstrap First)

| Feature | C Bootstrap Work | ShelbyC Phase |
|---------|-----------------|---------------|
| Must-use Result | Add check in `checker.c` — warn on unused Result ExprStmt | Phase 10 |
| Exhaustiveness checking | Add in `checker.c` or `cg_stmt.c` — match arm analysis | Phase 10 |
| Float equality warning | Add in `checker.c` — check `==`/`!=` on f32/f64 | Phase 11 |
| Signed/unsigned mixing | Add in `checker.c` — warn on mixed arithmetic | Phase 11 |
| Cyclomatic complexity | Add in `flow_analysis.c` — compute from CFG | Phase 11 |
| `SizeOf<T>()` built-in | Add in `cg_builtin.c` — `LLVMABISizeOfType` | Phase 5 |
| `OffsetOf<T>(field)` | Add in `cg_builtin.c` — `LLVMOffsetOfElement` | Phase 5 |
| `extern struct` for C FFI | Parser + checker + codegen — struct layout from LLVM DataLayout | Phase 5 |
| `CStr` global strings | Add in `cg_builtin.c` — `LLVMBuildGlobalStringPtr` | Phase 3 |
| Short-circuit `&&`/`||` | Already correct in C bootstrap | Phase 4 |
| No-alloc attribute | Parser (attribute), flow analysis (track allocations) | Phase 11+ |
| Comptime evaluation | Major feature — Phase 8 roadmap | Future |
| Comptime reflection | `T.Fields()`, `E.Variants()`, `DefineStruct` | Future |

---

## The Rule

Every function in the self-hosted compiler has a corresponding function in the C bootstrap. If they don't match, the self-hosted version is wrong. The C bootstrap is the source of truth until the Triple Test passes.

After the Triple Test: the self-hosted compiler IS the source of truth, and the C bootstrap is history.

---

*shelbyc-lang-proto was the first draft. This is the final version.*
