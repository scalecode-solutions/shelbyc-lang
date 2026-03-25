# Reflection Beyond C++26

Analysis of C++26 reflection (P2996R4), its limitations, and what a language designed from scratch can do differently.

---

## What C++26 Reflection Does

P2996R4 adds three mechanisms to C++:

- **`^` (reflect)**: lifts a program entity into a compile-time value of type `std::meta::info`
- **`[: :]` (splice)**: turns a reflection back into code
- **Metafunctions**: `name_of()`, `members_of()`, `type_of()`, `define_class()`, etc.

This lets you inspect types, iterate over members (with workarounds), and synthesize new types at compile time.

The committee explicitly says this is not the end-game: *"we expect it will be a useful core around which more powerful features will be added incrementally over time."*

---

## What C++26 Reflection Cannot Do

### 1. No Code Injection

You can inspect a struct's members. You cannot generate a new function from that inspection.

```cpp
// You CAN do this: query members
consteval auto get_fields() {
    return nonstatic_data_members_of(^MyStruct);
}

// You CANNOT do this: generate a serialize function
// There is no mechanism to emit:
//   void serialize(MyStruct& s) { write(s.field1); write(s.field2); }
// from reflected member information
```

P2237R0 proposes code injection to fill this gap. Not in C++26. Future work.

The fundamental limitation: C++ treats reflection (inspection) and generation (emission) as separate capabilities requiring separate mechanisms. Inspection shipped. Generation didn't.

### 2. No Range Splicing

You can splice ONE reflection into code. You cannot splice a RANGE (like "all members of this struct") in one operation.

```cpp
// You CAN do this:
constexpr auto r = ^int;
typename[:r:] x = 42;  // splices one type

// You CANNOT do this:
// [: ... members_of(^MyStruct) :]  // splice all members
// There is no variadic expansion of reflected ranges
```

Every example that iterates over members requires either `template for` (P1306R2, also not in C++26) or a workaround helper called `replicator`.

Deferred because *"range splicing of dependent arguments is at least an order of magnitude harder to implement."* The interaction between variadic expansion and C++'s existing template instantiation machinery is unsolved.

### 3. No Expression Reflection

```cpp
^int     // OK: reflects a type
^printf  // OK: reflects a function
^(x + 1) // NOT SUPPORTED: can't reflect arbitrary expressions
```

Intended for C++26 with the syntax `^(expr)`. Dropped because side effects in constant evaluation made it infeasible. If the expression has side effects, reflecting it would require deciding when/whether to evaluate it. Unresolved.

### 4. No Constructor/Destructor Splicing

You can reflect on a class's constructors via `members_of()`. You cannot splice them into a call. You get the reflection but can't use it to invoke construction.

Deferred. Acknowledged as "sensible" but not implemented.

### 5. No Dependent Concept Splicing

```cpp
template <[:concept_reflection:]  T>  // NOT ALLOWED
void f(T x);
```

The compiler cannot determine whether the spliced concept constrains a type parameter or a non-type parameter. Ambiguous in C++'s grammar. Deferred.

### 6. No Designated Initializer Splicing

```cpp
MyStruct s = { .[:field_reflection:] = value };  // NOT ALLOWED
```

Cannot use a reflected member as a designated initializer. Would require the parser to resolve reflections during aggregate initialization. Deferred pending implementation experience.

### 7. Expansion Statements Not In C++26

`template for` (P1306R2) — the ability to iterate over a compile-time range and expand the loop body per element — is not in C++26 despite being required by most reflection examples. The proposal provides a `replicator` workaround that's verbose and limited.

### 8. String Literals in Templates

Non-type template parameters cannot be string literals directly. Named tuples and other string-parameterized types require a `fixed_string` workaround type. Basic compile-time string manipulation is painful.

---

## Why These Limitations Exist

Every limitation traces back to one root cause: **C++ is retrofitting compile-time metaprogramming onto a language with 40 years of existing template machinery, overload resolution, and parsing rules.**

- Range splicing conflicts with template variadic expansion
- Expression reflection conflicts with constant evaluation side effects
- Constructor splicing conflicts with overload resolution
- Concept splicing conflicts with template parameter parsing
- Code injection conflicts with the separation between declarations and definitions

The proposal is carefully scoped to avoid breaking any existing C++ feature. Every deferred item was deferred because it interacts badly with something that already exists.

---

## What a Clean-Slate Language Can Do

Without C++'s baggage, every limitation dissolves. Not because the problems are easy — but because there's no existing machinery to conflict with.

### Code Injection = Comptime Functions That Return Types

C++ needs a separate injection mechanism because templates and consteval are different worlds.

A comptime function that returns a type IS code injection:

```
comptime fn MakeSerializer<T>() type {
    fields := T.Fields();
    // Build a new type with a serialize method
    // that writes each field
    return DefineStruct(/* ... */);
}
```

A comptime function that builds a function body IS code generation. No separate injection mechanism. No new syntax. A function that runs at compile time and produces code.

### Range Splicing = comptime for

C++ can't splice a range because template expansion doesn't support it.

`comptime for` iterates over a compile-time array and unrolls:

```
fn ToJson<T>(obj T) str {
    result := "{";
    comptime for field in T.Fields() {
        result = result + "\"" + field.Name + "\": ";
        result = result + Serialize(obj.Field(field));
    }
    return result + "}";
}
```

Each iteration emits different code based on the field. The loop is fully unrolled at compile time. No range splicing operator needed — it's a loop.

### Expression Reflection = Comptime Values

In C++, `^(x + 1)` is problematic because expressions can have side effects.

In a comptime context, expressions ARE values:

```
comptime x := 3 + 4;       // evaluated at compile time, x = 7
comptime t := TypeOf(x);    // t = i32
```

No separate reflection operator for expressions. If you're in a comptime context, everything is evaluated and inspectable.

### Constructor Splicing = Not Needed

C++ needs constructor splicing because construction is a special operation with overload resolution, implicit conversions, and copy/move semantics.

Struct literal syntax eliminates the problem:

```
instance := MyStruct{ field1: value1, field2: value2 };
```

There's no constructor to splice. Construction is always explicit field initialization. Comptime code that builds struct literals builds construction automatically.

### Dependent Concept Splicing = Generic Constraints

C++ can't splice a concept as a template constraint because the parser can't tell if it constrains a type or a value.

Generic constraints as expressions:

```
fn Sort<T: Comparable>(arr []T) {
    // T must satisfy Comparable
}

comptime fn MakeConstrained<T, C>() type {
    // C is a constraint, T is a type
    // No ambiguity — constraints are explicitly applied
}
```

The constraint is part of the generic parameter syntax, not a separate concept declaration that gets spliced.

---

## The Real Problems (What C++ Reveals About Reflection in General)

Stripped of C++'s baggage, the C++26 proposal reveals the actual hard problems in compile-time metaprogramming:

### 1. Compile-Time String Manipulation

Every reflection use case needs string building:
- JSON serialization: build field name strings
- SQL ORM: build column name queries
- Debug output: build human-readable type descriptions
- Code generation: build function names from type names

C++26 gives you `std::string_view` from `name_of()` but building NEW strings at compile time is painful. No concatenation, no formatting, no interpolation in consteval contexts (limited `constexpr` string support).

Requirements:
- String concatenation at compile time
- String interpolation with comptime values
- String comparison and pattern matching
- String → identifier conversion (use a string to name a generated type/field)

### 2. Type Synthesis

`define_class()` in C++26 creates new types from a vector of member specifications. It works but it's clunky — an imperative API for building types.

Better approaches:
- Comptime functions that return `type` values directly
- Type literals: `struct { x: i32, y: i32 }` as a comptime expression
- Type algebra: `TypeA | TypeB` for unions, `TypeA & TypeB` for intersections
- Incremental building: start with a type, add fields/methods conditionally

### 3. Annotation/Attribute Access

C++26 can reflect on standard attributes (`[[nodiscard]]`, `[[deprecated]]`) but not user-defined ones. Most metaprogramming use cases need custom annotations:

```
#[serialize]
#[validate(min: 0, max: 100)]
struct Config {
    #[json_name("server_port")]
    port i32;
}
```

Requirements:
- User-defined attributes with arguments
- Attributes accessible via `T.Attributes()` and `field.Attributes()` at comptime
- Attribute values as comptime-evaluatable expressions
- Attributes on types, fields, functions, parameters, enum variants

### 4. Compile-Time I/O (Controversial)

Some use cases need to read external data at compile time:
- Parse a protobuf `.proto` file and generate types
- Read a JSON schema and generate validation structs
- Read a SQL migration and generate the corresponding types

C++26 explicitly prohibits this (hermetic compilation). Zig also prohibits comptime I/O.

The argument for: it eliminates entire code generation tools (protoc, sqlc, etc.).
The argument against: non-hermetic compilation breaks reproducibility and caching.

Middle ground: `comptime import "schema.proto"` where the imported file is declared as a build dependency and its hash is part of the compilation cache key. The compilation is reproducible because the input file is tracked.

### 5. Reflection on Control Flow

No existing reflection system can reflect on control flow within a function body. You can inspect declarations (types, functions, fields) but not implementation (if/else, loops, match arms).

Use cases:
- Auto-generating state machine diagrams from match expressions
- Verifying that all error paths are handled (exhaustiveness on control flow, not just pattern matching)
- Extracting the CFG for analysis tools

This is where reflection meets flow ownership. The compiler already builds a CFG for ownership analysis. Exposing that CFG to comptime functions would enable user-defined program analysis.

### 6. Cross-Module Reflection

C++26 reflection works within a translation unit. Reflecting on types from other modules requires those modules to be compiled first. The interaction between reflection and C++20 modules is unspecified.

Requirements:
- Reflect on types from imported modules
- Reflect on the import graph itself (which modules depend on which)
- Cross-module type synthesis (generate a type in module A based on types in module B)

### 7. Reflection Hygiene

When comptime code generates new declarations, name collisions are possible. Scheme solved this with hygienic macros. Rust's proc macros have span hygiene.

Requirements:
- Generated names shouldn't collide with user names
- Generated code should have clear provenance (debugging: where did this generated function come from?)
- Error messages for generated code should point to the generation site, not the output

---

## Implementation Priority for ShelbyC

Based on what C++26 reveals about real-world needs:

### Must-have for comptime (Phase 8)

1. **`T.Fields()`, `T.Methods()`, `E.Variants()`** — basic type introspection
2. **`comptime for`** — iteration over comptime arrays (replaces range splicing)
3. **Comptime string operations** — concatenation, interpolation, comparison
4. **`DefineStruct(fields)`** — type synthesis from comptime data
5. **Comptime function evaluation** — any function callable at compile time if inputs are known

### Should-have

6. **User-defined attributes** with comptime-accessible values
7. **`SizeOf<T>()`, `AlignOf<T>()`, `OffsetOf<T>(field)`** — layout queries
8. **Comptime type algebra** — union types, optional wrapping, generic instantiation
9. **`TypeName<T>()`** — string representation of types for debug/serialization

### Nice-to-have

10. **Comptime file import** — read external files as build dependencies
11. **CFG reflection** — expose the control flow graph to comptime analysis
12. **Cross-module reflection** — inspect types from imported modules
13. **Hygienic name generation** — prevent collisions in generated declarations

### Not needed

- `^` reflect operator — types are already comptime values
- `[: :]` splice operator — comptime values are directly usable
- `std::meta::info` opaque type — `type` is first-class
- `template for` — `comptime for` is the same thing with less syntax
- Constructor splicing — struct literals
- Concept splicing — generic constraints

---

## The C++26 Committee's Own Assessment

From the proposal:

> *"This proposal is not intended to be the end-game as far as reflection and compile-time metaprogramming are concerned. Instead, we expect it will be a useful core around which more powerful features will be added incrementally over time."*

They shipped what they could without breaking C++. The deferred features (code injection, range splicing, expression reflection) are each "at least an order of magnitude harder" because of interactions with existing C++ machinery.

A language without that machinery doesn't have those interactions. The features aren't harder — they're just features.

---

*C++26 reflection is a remarkable engineering achievement given the constraints. It's also a catalog of problems that a clean-slate language can solve more naturally. The limitations aren't in the ideas — they're in the 40 years of accumulated syntax that the ideas have to navigate around.*
