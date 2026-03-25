# Type System Alignment: C Bootstrap vs ShelbyC

**Date:** March 24, 2026
**Purpose:** Document the exact alignment between the C bootstrap (source of truth) and the ShelbyC self-hosted compiler types. Every difference is intentional and documented.

---

## Enum Counts

| Enum | C Bootstrap | ShelbyC | Status |
|------|-------------|---------|--------|
| TokenKind | 131 variants | 131 variants | **Exact match** |
| NodeKind | 92 variants + COUNT sentinel | 91 variants | **Match** (no sentinel needed — Vec.Len() replaces COUNT) |
| TypeKind | 48 variants | 57 variants | **ShelbyC superset** (includes sync/concurrency types grouped differently) |

### TokenKind: 131 = 131
Identical ordering. Tag values match. Both start from Eof=0 through Hash=129, with Ident=14 as the identifier token.

### NodeKind: Naming Differences (Same Tag Values)
| C Bootstrap | ShelbyC | Tag |
|-------------|---------|-----|
| `ND_IDENT` | `IdentNode` | 8 |
| `ND_RAW_STR` | `RawStrNode` | 70 |
| `ND_MULTILINE_STR` | `MultilineStrNode` | 69 |
| `ND_COUNT` | (none — not needed) | 92 |

All other node kinds have the same tag values via enum ordering.

### TypeKind: Casing Differences
| C Bootstrap | ShelbyC | Notes |
|-------------|---------|-------|
| `TY_RWLOCK` | `TyRwLock` | Cosmetic — tag values match |
| `TY_TASKSCOPE` | `TyTaskScope` | Cosmetic — tag values match |
| `TY_WAITGROUP` | `TyWaitGroup` | Cosmetic — tag values match |

Tag values are what matter for type checking and codegen. Names are cosmetic.

---

## Struct Comparison: MonoEntry

The critical struct for Bug #21 (generic fn returning generic struct).

### C Bootstrap
```c
struct MonoEntry {
    AstNode    *fn_decl;         // original generic function AST node
    Type      **concrete_types;  // array of concrete Type* for each type param
    int         type_param_count;
    const char *mangled_name;    // e.g., "Swap__i32"
    uint32_t    mangled_len;
    Type       *mono_fn_type;    // FULLY SUBSTITUTED function type
    MonoEntry  *next;            // linked list (iteration)
    MonoEntry  *hash_next;       // hash chain (O(1) lookup)
};
```

### ShelbyC
```
struct MonoEntry {
    fn_name_start i32;     // generic function name (source position)
    fn_name_len   i32;
    mangled_start i32;     // mangled name (source position)
    mangled_len   i32;
    node_idx      i64;     // original AST node index
    concrete_type i32;     // first concrete type (checker type index)
    concrete_start i64;    // index into checker.type_children for ALL concrete types
    concrete_count i32;    // number of type params
    fn_type_idx   i32;     // SUBSTITUTED function type (checker type index)
}
```

### Key Differences

| Aspect | C Bootstrap | ShelbyC | Rationale |
|--------|-------------|---------|-----------|
| Function reference | `AstNode *fn_decl` (pointer) | `node_idx i64` (index) | Index is bounds-checkable, no dangling pointer risk |
| Concrete types | `Type **concrete_types` (pointer array) | `concrete_start i64` + `concrete_count i32` (indices into type_children) | Index-based, same data different access pattern |
| Mangled name | `const char *mangled_name` (heap string) | `mangled_start i32` + `mangled_len i32` (source offsets) | Avoids heap allocation for name storage |
| Substituted fn type | `Type *mono_fn_type` (pointer) | `fn_type_idx i32` (index) | **Both present — critical for Bug #21 fix** |
| Collection | Linked list (`next`, `hash_next`) | `Vec<MonoEntry>` | Vec is simpler, linked list allows O(1) hash lookup |

### Why Indices Over Pointers (JSF Alignment)

The ShelbyC design uses indices (`i32`/`i64`) instead of pointers for all cross-references:

- **Bounded**: `idx >= 0 && idx < vec.Len()` — can be range-checked at any point
- **Deterministic**: Types/nodes are append-only (never removed), so indices are stable
- **No dangling references**: Unlike pointers, indices can't point to freed memory
- **Auditable**: Plain integer values — printable in error messages, serializable
- **JSF Rule 170 compliant**: No pointer arithmetic risks

The C bootstrap uses pointers because C requires them for the arena allocator pattern. ShelbyC uses indices because it's the safer design. Both are valid; indices are preferred.

---

## Struct Comparison: SCType vs Type

### C Bootstrap (`types.h`)
```c
struct Type {
    TypeKind kind;
    union {
        struct { Type *inner; bool is_mut; } reference;
        struct { Type *elem; int64_t size; } array;
        struct { Type **params; int param_count; Type *ret; ... } func;
        struct { TypeField *fields; int field_count; char *name; ... } struc;
        struct { TypeVariant *variants; int variant_count; ... } enu;
        // ... more union members
    } u;
};
```

### ShelbyC (`types.smc`)
```
struct SCType {
    kind TypeKind;
    name_start i32;
    name_len   i32;
    inner i32;            // inner type index
    param_count i32;      // fn params, struct fields, enum variants
    ret_type i32;         // fn return, map val, result err
    children_start i64;   // index into type_children
    array_size i64;
    is_mut i32;
    node_idx i64;         // back-pointer to AST node
}
```

### Key Difference: Union vs Flat

The C bootstrap uses a tagged union — each Type kind has its own struct layout within the union. This is memory-efficient but requires accessing different field names per kind (`t->u.struc.fields` vs `t->u.func.ret`).

ShelbyC uses a flat struct — all fields exist on every type, with unused fields set to -1. This wastes ~40 bytes per type but:
- No union dispatch needed
- Same field names for all kinds (`st.inner`, `st.ret_type`)
- Simpler codegen (no offset calculation per kind)
- `inner` serves as the single inner type for Ref, Ptr, Vec, Option, etc.
- `ret_type` serves as the second type for Map (key→val), Result (ok→err), Fn (→return)

For a compiler processing ~1000 types, the memory overhead is ~40KB. Negligible.

---

## What The C Bootstrap Has That ShelbyC Must Add

### 1. TyTypeParam Entries
The C bootstrap creates `TY_TYPE_PARAM` Type entries during type param registration. Each has an `index` field that maps to `mono_concrete[index]` during substitution.

ShelbyC currently does NOT create TyTypeParam entries. Type param substitution is done by name matching in the codegen. This is Bug #21's root cause.

**Fix**: Create TyTypeParam entries in the checker during Pass 2 when processing generic function/struct type params.

### 2. TypeSubstitute for TyStruct
The C bootstrap's `checker_substitute_type` handles ALL type kinds recursively. ShelbyC's `TypeSubstitute` is missing the TyStruct (kind 23) case.

**Fix**: Add `if k == 23` to TypeSubstitute — substitute `inner` and `ret_type` (which carry the first two type params for generic structs).

### 3. mono_fn_type Population
The C bootstrap populates `me->mono_fn_type` at the call site where monomorphization is detected. It calls `checker_substitute_type` on the generic function type to produce a fully-resolved concrete function type.

ShelbyC has `fn_type_idx` on MonoEntry but doesn't populate it correctly yet. `RecordMono2` needs to call `TypeSubstitute` on the function's type to produce the substituted version.

**Fix**: In `RecordMono2`, look up the generic function's type, call `TypeSubstitute` to get the concrete function type, store the result in `me.fn_type_idx`.

---

## Codegen Resolution Path Comparison

### C Bootstrap: Type-System Resolution
```
ForwardDeclareMonos:
  ft = me->mono_fn_type                    // already substituted
  ret_lt = cg_llvm_type(cg, ft->ret)       // resolve through Type system
    → TY_STRUCT: iterate fields[]
      → TY_TYPE_PARAM: lookup mono_concrete[index]  // indexed substitution
      → returns concrete LLVM type
  LLVMFunctionType(ret_lt, ptypes, ...)     // correct struct size
```

### ShelbyC Proto: AST-Node Resolution (BROKEN)
```
ForwardDeclareMonos:
  ret_ty = ResolveReturnType(fn_nd.c2)      // AST node for return type
    → TypeGeneric handler
      → finds StructDecl symbol
      → iterates AST field nodes
      → ResolveTypeNode(field.c0)            // name matching for "T"
        → may find template TyStruct (inner=-1)
        → returns void for unresolved T
  LLVMFunctionType(ret_ty, ...)              // void field → LLVM abort
```

### ShelbyC v2: Type-System Resolution (CORRECT)
```
ForwardDeclareMonos:
  fn_ti = me.fn_type_idx                    // already substituted
  ft = checker.types.Get(fn_ti)
  ret_lt = ResolveTypeNode2(ft.ret_type)    // resolve through checker types
    → TyStruct: anonymous LLVM type from fields
      → field types already concrete (substituted by TypeSubstitute)
      → no name matching needed
  LLVMFunctionType(ret_lt, ptypes, ...)     // correct struct size
```

---

*This document is the contract between the C bootstrap and the ShelbyC rewrite. Every discrepancy listed here is either intentional (indices vs pointers) or a bug to fix (TyTypeParam, TypeSubstitute, mono_fn_type).*
