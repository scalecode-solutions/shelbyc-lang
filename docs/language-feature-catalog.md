# Language Feature Catalog

Best ideas from across programming language history. Raw reference material.

---

## Erlang/OTP

### Supervision Trees

Hierarchical process trees where supervisor processes monitor workers. Four restart strategies:

- **one_for_one**: children are independent. One dies, only it restarts.
- **one_for_all**: children are interdependent. One dies, all restart in boot order.
- **rest_for_one**: linear dependency chain. One dies, all processes started after it also restart.
- **simple_one_for_one**: homogeneous dynamic worker pool. All children share one spec.

Each supervisor has `{MaxRestarts, MaxSeconds}`. If exceeded, the supervisor itself crashes, escalating to its parent. This continues up the tree. The escalation is a circuit breaker, not a bug.

Child restart types: `permanent` (always restart), `transient` (restart on abnormal exit only), `temporary` (never restart). Shutdown specs control graceful termination: timeout, infinity, or brutal_kill.

The manager-worker pattern: a manager process sits as a sibling to a `temporary` supervisor, monitors it, and applies custom policies (exponential backoff, circuit breaking) before restarting the sub-tree.

### Let It Crash

Jim Gray's research: 131 of 132 production errors are heisenbugs (transient). Only 1/132 is a bohrbug (deterministic). Restarting from known-good state eliminates the vast majority.

Defensive code is dangerous: try/catch blocks are rarely tested, often wrong, and can mask the real problem by continuing with corrupted state. An incorrect catch handler that silently swallows an error is worse than a crash.

Prerequisites: process isolation (crash can't corrupt others), automatic resource cleanup (file descriptors, sockets cleaned on death), supervision (crashed process restarted from known-good state). Without all three, "let it crash" is just "let it break."

### Binary Pattern Matching

Pattern match on binary data at arbitrary bit granularity:

```erlang
<<Version:4, HeaderLen:4, ServiceType:8, TotalLen:16,
  ID:16, Flags:3, FragOffset:13,
  TTL:8, Protocol:8, Checksum:16,
  SrcAddr:32, DestAddr:32,
  RestOfPacket/binary>> = Packet
```

Each segment: `Value:Size/TypeSpecifierList`. Types: integer, float, binary, bitstring. Signedness: signed/unsigned. Endianness: big/little/native. Dynamic sizes: a matched variable can be the size of a subsequent segment: `<<Len:16, Payload:Len/binary, Rest/binary>>`.

Handles protocols where fields don't fall on byte boundaries (3-bit flags + 13-bit fragment offset in IPv4).

### OTP Behaviours

Codified concurrency patterns as callback modules. The framework handles message receive loops, error handling, sys protocol compliance, and hot code upgrade hooks.

**gen_server**: client-server pattern. `init/1`, `handle_call/3` (sync), `handle_cast/2` (async), `handle_info/2`, `terminate/2`, `code_change/3`.

**gen_statem**: finite state machines. Two modes: `state_functions` (one callback per state) and `handle_event_function` (single callback for all states). **Event postponement**: `{postpone, true}` defers the current event to be retried in a future state. The framework retries postponed events automatically when the state changes. Eliminates race conditions in protocol implementations.

**gen_event**: event manager with multiple handler modules, each with independent state. Built-in pub/sub at the process level.

### Process Isolation and Per-Process GC

Each process: private heap, private stack, private garbage collector. Zero shared mutable state. Messages copied between heaps. Large binaries (>64 bytes) reference-counted in a shared binary heap — only the pointer is copied.

Per-process GC: minor collections for young objects, major collections infrequently. No global stop-the-world pause. Short-lived processes that complete before GC triggers are deallocated wholesale — zero GC cost.

Fresh process: ~300-500 bytes. Enables millions of concurrent processes.

### Reduction Counting

Preemption without timer interrupts. Each process gets ~2000 reductions (roughly one per function call). When the budget is exhausted, the process yields. Deterministic, fair scheduling cheaper than timer-based preemption.

Per-core run queues with work stealing. Four priority levels (low, normal, high, max). Priority levels ensure supervisors always get CPU time even under load.

### ETS (Erlang Term Storage)

Concurrent in-memory hash tables and ordered trees outside the GC. Table types: set (unique keys, O(1)), ordered_set (balanced tree, O(log N) with range queries), bag, duplicate_bag.

Access modes: protected (owner writes, anyone reads), public (anyone reads/writes), private (only owner).

Ownership and heir: each table has an owning process. If the owner dies, the table transfers to a designated heir process. State survives crashes without persistence.

Match specifications: compiled pattern language for querying, atomically deleting, or atomically updating entries.

### Hot Code Reloading

VM maintains two versions of every module simultaneously (current and old). When a new version loads, current becomes old, new becomes current. Processes switch to new code on fully qualified function calls.

Stateful processes use a three-step protocol: suspend, transform state via `code_change/3` callback, resume.

### Two-Tier FFI

**Ports**: communicate with an external OS process over stdin/stdout. Complete isolation — if external process crashes, VM continues. Slow (OS context switches, serialization).

**NIFs**: dynamically link C into the VM. Fast (no serialization, no context switch). Dangerous (crash in NIF code takes down entire VM).

**Resource objects**: C-side memory tied to GC lifecycle. Automatically freed when last Erlang reference is garbage collected.

**Dirty schedulers**: separate OS threads for long-duration NIFs, classified as CPU-bound or IO-bound. Prevents blocking the normal scheduler.

### Process Registry

Register processes under names. Names automatically unregistered on process death.

gproc extended registry: `{Type, Scope, Key}` tuples. Types: name (unique), property (non-unique), counter, aggregate counter. Scopes: local or global. Enables pub/sub, service discovery, distributed metrics. `await` blocks until a process registers under a name — startup coordination without polling.

---

## Ada / SPARK / Eiffel

### Range Types and Constrained Subtypes (Ada)

```ada
type Percent is range 0 .. 100;
type Temperature_C is digits 6 range -273.15 .. 10_000.0;
subtype Dice_Throw is Integer range 1 .. 6;
```

Any assignment outside the range raises `Constraint_Error`. The compiler knows the valid domain of every value. You cannot pass a `Percent` where a raw `Integer` is expected without explicit conversion.

### Derived Types (Ada)

```ada
type Distance_Meters is new Float;
type Distance_Feet  is new Float;
D : Distance_Meters := 100.0;
F : Distance_Feet   := D;  -- COMPILE ERROR
```

Genuinely incompatible types from the same underlying representation. All operators inherited but constrained to their own type. Would have prevented the Mars Climate Orbiter disaster.

### Subtype Predicates (Ada 2012)

```ada
type Even is new Integer with Dynamic_Predicate => Even mod 2 = 0;
subtype Not_Null_Integer is Integer with Dynamic_Predicate => Not_Null_Integer /= 0;
subtype Weekend is Day with Static_Predicate => Weekend in Saturday | Sunday;
```

`Static_Predicate` checked at compile time. `Dynamic_Predicate` checked at assignment, parameter passing, object creation.

### Representation Clauses (Ada)

Specify exact bit layout of a record, then use it as a normal typed value:

```ada
for Program_Status_Word use record
   System_Mask     at 0 range 0 .. 7;
   Protection_Key  at 0 range 10 .. 11;
   Machine_State   at 0 range 12 .. 15;
   Interrupt_Cause at 0 range 16 .. 31;
   Inst_Address    at 4 range 8 .. 31;
end record;
```

The compiler generates shift/mask code. Type safety AND bit-precise hardware control simultaneously.

### Discriminated Records (Ada)

Variant records where discriminant fields determine which other fields exist:

```ada
type Expr (Kind : Expr_Kind) is record
   case Kind is
      when Bin_Op => Left, Right : Expr_Access;
      when Num    => Val : Integer;
   end case;
end record;
```

Discriminants can control array sizes within the record: `type Stack (Max : Natural) is record Items : Item_Array (1 .. Max); end record;`. Each instance sized precisely.

### Tasking with Rendezvous (Ada)

First-class tasks with `select` statement for waiting on multiple entries simultaneously. Branches are mutually exclusive. Each entry has a FIFO queue of waiting callers.

### Protected Objects with Barriers (Ada)

Passive shared state with built-in read/write locking. Functions allow concurrent reads. Procedures and entries get exclusive access. Entries have boolean barriers — the runtime requeues callers automatically when barriers are false and re-evaluates them when state changes. Eliminates manual condition-variable signaling.

### Ravenscar Profile (Ada)

One pragma restricts concurrency to a formally analyzable subset: no dynamic task creation, no abort, no rendezvous, no relative delays, at most one entry per protected object, at most one queued caller, ceiling locking protocol, FIFO within priorities.

Enables Rate Monotonic Analysis — mathematically proving all tasks meet their deadlines. Referenced by DO-178B/C (avionics), ECSS (space).

### Generic Formal Contracts (Ada)

Declare exactly what you need from a type parameter:

```ada
generic
   type Element is private;
   type Index is (<>);
   type Array_Type is array (Index) of Element;
   with function "<" (L, R : Element) return Boolean;
procedure Sort (A : in out Array_Type);
```

Two-phase model: errors in the generic body caught when compiled, errors in instantiation caught at call site. No C++-style template error cascade. Ada had this in 1983. C++ got concepts in 2020.

### SPARK Formal Verification

Proves absence of ALL runtime errors: no buffer overflows, no integer overflow, no division by zero, no null dereferences, no range violations, no discriminant check failures.

Five verification levels: Stone (no aliasing), Bronze (no uninitialized reads), Silver (no runtime errors), Gold (key functional properties), Platinum (complete specification).

### SPARK Flow Analysis (Depends Contracts)

```ada
procedure Swap (X, Y : in out Integer) with
   Global  => null,
   Depends => (X => Y, Y => X);
```

`Global` specifies every global variable a subprogram touches. `Depends` specifies which outputs depend on which inputs. The tool verifies both are correct and complete. Catches: uninitialized reads, dead stores, incorrect parameter modes, information leaks.

### SPARK Ghost Code

Program entities that exist solely for specification and proof, guaranteed removed from the compiled binary:

```ada
function Is_Sorted (A : Array_Type) return Boolean with Ghost;

procedure Sort (A : in out Array_Type) with
  Post => Is_Sorted (A)
is
   A_Old : constant Array_Type := A with Ghost;
begin
   for I in A'Range loop
      pragma Loop_Invariant (Is_Sorted (A (A'First .. I)));
   end loop;
end Sort;
```

Ghost entities cannot affect non-ghost code — verified syntactically. Ghost code can be non-executable — mathematical models used only in proofs.

### Eiffel Design by Contract

```eiffel
deposit (sum: INTEGER)
   require
      non_negative: sum >= 0
   do
      balance := balance + sum
   ensure
      balance_increased: balance = old balance + sum
   end

invariant
   non_negative_balance: balance >= 0
```

`require` (preconditions), `ensure` (postconditions), `old` (values at entry), `invariant` (class invariants checked after creation and after every exported routine). Each clause labeled for precise error reporting.

### Contract Inheritance (Eiffel)

When a subclass overrides a method:
- Preconditions can only be weakened (`require else` — ORs with parent)
- Postconditions can only be strengthened (`ensure then` — ANDs with parent)
- Class invariants always inherited and ANDed

Enforces Liskov Substitution Principle at the compiler level.

### Per-Class Assertion Monitoring (Eiffel)

Granular runtime checking levels: no checking, preconditions only, pre+post, add invariants, add all checks. Set per class — hot-path classes unchecked, suspicious classes fully monitored.

### Eiffel Void Safety

Attached types (default) guaranteed non-void. Detachable types (`?`) may be void, cannot be dereferenced without checking. Certified Attachment Patterns (CAPs) certify that a reference is non-void within a scope.

### SCOOP (Eiffel)

Single keyword `separate` indicates an object may live on a different processor. Calls on separate objects are asynchronous. Preconditions on routines with separate arguments become wait conditions. Guarantees no data races and deadlock prevention through the runtime scheduler.

---

## Zig / Pony / Vale / Hylo / Odin

### Zig Comptime

Compile-time interpreter embedded in the compiler. Types are first-class values of type `type`. Generics are functions that take and return types. No separate template syntax.

Deliberate restrictions: no host leakage (cross-compilation safe), no string injection (no `mixin`-style eval), no DSLs, no method injection on generated types, no I/O (hermetic builds).

### Zig Error Unions and Inferred Error Sets

Error sets are named enums of error values. Error unions fuse errors into the type: `fn open(path: []const u8) FileError!File`. Inferred error sets: write `!T` and the compiler infers all possible errors through call chains automatically.

`try` unwraps and propagates. `catch` handles locally. `errdefer` runs cleanup only on error return. Error return traces in debug builds track the full chain of `try` sites.

### Zig Explicit Allocator Interface

No global allocator. Every function that allocates accepts an `std.mem.Allocator` parameter. Type-erased interface with alloc, resize, remap, free. Standard library ships: page allocator, fixed buffer allocator, arena allocator, general purpose (debug with use-after-free/leak detection), SMP allocator.

Every allocation visible in function signatures. Zero hidden allocations in the standard library.

### Zig Sentinel-Terminated Types

Null-termination encoded in the type system: `[N:0]u8` (array terminated by zero), `[:0]u8` (slice terminated by zero), `[*:0]u8` (pointer terminated by zero). String literals have type `*const [N:0]u8`. C interop for null-terminated strings is type-safe without casts.

### Zig Build System

`build.zig` is a Zig program, not a config file. Compiled to a native binary, then executed. Constructs a DAG of steps that the build runner executes concurrently. Full access to comptime, conditionals, and the standard library.

### Pony Reference Capabilities

Six capabilities encoding read/write permissions and sharing constraints:

| Cap | Read | Write | Shareable Across Actors | Aliases | Property |
|---|---|---|---|---|---|
| **iso** | Yes | Yes | Yes (transferable) | None | Isolated unique reference |
| **trn** | Yes | Yes | After conversion to val | box only | Transitional: mutable now, immutable later |
| **ref** | Yes | Yes | No | Yes | Normal mutable |
| **val** | Yes | No | Yes | Yes | Deeply immutable, freely shareable |
| **box** | Yes | No | No | Yes | Read-only view |
| **tag** | No | No | Yes | Yes | Identity only |

Each capability defined by what it *denies to all other references*. Data-race freedom is provable at compile time without locks, atomics, or message copying.

### Pony Recover Blocks

Lexical scope where only iso, val, and tag references from the enclosing scope can enter. Inside: build mutable structures with ref. On exit: result lifted to a more restrictive capability. Build mutable, seal immutable — safely.

### Pony Viewpoint Adaptation

Algebraic operator on capabilities for nested data access: `box->ref = box`, `ref->ref = ref`, `val->ref = val`. `this->` notation in signatures means "the capability of this field as seen by the receiver's actual capability." One function works correctly regardless of whether the receiver is ref, val, or box.

### Pony Per-Actor GC (ORCA)

Fully concurrent, per-actor garbage collection. Collection runs between behaviors (message handler invocations). No global stop-the-world pause. Dead actor cycles collected by a dedicated cycle detector using deferred, distributed, weighted reference counting.

### Vale Generational References

Every heap object has an 8-byte current generation integer. Every pointer stores the generation it saw when created. On dereference: assert `pointer.generation == object.generation`. Mismatch = use-after-free caught at runtime. When an object is freed, its generation increments.

Overhead: 2-10.84%. Allows patterns borrow checkers reject (observers, back-references, graphs with cycles).

### Vale Region Borrow Checker

Regions operate on groups of objects. A frozen (immutable) region eliminates all generational reference checks — zero-cost borrowing. Pure functions make all parameters immutable, eliminating every generation check. Isolates: a data hierarchy where nothing inside points out and nothing outside points in. Regions are opt-in — code without them still gets safety through generational references.

### Vale Perfect Replayability

Deterministic replay as a language property: eliminate undefined behavior, remove nondeterminism sources, record all FFI boundary inputs. Replay by feeding the recording instead of real I/O. Can add logging and pure functions to replayed program without invalidating recording. Can refactor code and still replay.

### Hylo Mutable Value Semantics

All types are value types. No reference types. References exist only at function boundaries as parameter passing modes and cannot be stored in variables or struct fields.

Four parameter conventions:

| Convention | Keyword | Semantics |
|---|---|---|
| Immutable borrow | `let` (default) | Caller retains ownership, callee reads |
| Mutable borrow | `inout` | Caller retains ownership, callee gets exclusive mutable access |
| Move/consume | `sink` | Ownership transfers to callee |
| Initialization | `set` | Callee writes into uninitialized binding |

The `set` convention models uninitialized memory in function signatures.

### Hylo Law of Exclusivity

At any point: a value is either being read by one or more readers, or written by exactly one writer, but never both. Enforced through parameter conventions rather than lifetime tracking. No storable references means no lifetime annotations needed.

### Hylo Subscripts

Coroutine-like projections that yield a value, suspend, let the caller operate, then resume for cleanup:

```hylo
subscript min_of(_ x: inout Int, _ y: inout Int): Int {
  if y < x { yield &y } else { yield &x }
  // code after yield runs when caller is done
}
```

Property syntax with mutable borrow + cleanup semantics.

### Odin Implicit Context Parameter

Every procedure receives an implicit `context` pointer. Contains: default allocator, temp allocator, logger, assertion handler, RNG, user data. Copy-on-write semantics: modifying context in a scope creates a local copy. Change how a library allocates by reassigning `context.allocator` before calling into it.

### Odin SOA Struct Types

Single annotation transforms memory layout:

```odin
Vector3 :: struct { x, y, z: f32 }
aos: [1000]Vector3           // x,y,z,x,y,z,...  (AoS)
soa: #soa[1000]Vector3       // x,x,...,y,y,...,z,z,...  (SoA)
```

`soa[0].x` and `soa.x[0]` both work. Works with slices and dynamic arrays. `soa_zip`/`soa_unzip` for conversion.

### Odin Design Philosophy: Removal

No methods, no exceptions, no capturing closures, no operator overloading, no UFCS, no implicit overloading, no implicit type conversions, no `while` loops. Principle: most programmers spend most time reading code, not writing it.

---

## Haskell / OCaml / Common Lisp / Effects / APL

### Typeclasses (Haskell)

Compiled to implicit dictionary parameters — records of function pointers. Not dispatch on the object (OOP vtables) but dispatch on the type. Dictionary resolved at compile time.

Solve three problems OOP interfaces cannot:
- **Return-type polymorphism**: `fromInteger :: Num a => Integer -> a` creates a value with no input to dispatch on.
- **Retroactive conformance**: declare existing types implement new interfaces without modifying originals.
- **Multi-parameter type classes**: dispatch on multiple types simultaneously.

Static dispatch by default (monomorphization). Dynamic dispatch available via existential types (fat pointer with dictionary).

### Phantom Types (Haskell)

Type parameter that carries no runtime data — purely compile-time marker. `SafeString = Tagged String Safe` where `Safe` is unexported. The only way to get a `SafeString` is through `sanitize`. Zero-cost compile-time state machines encoding whether data is sanitized, validated, authenticated.

### GADTs (Haskell)

Each constructor can specify a different instantiation of the type parameter:

```haskell
data Term a where
  Lit    :: Int -> Term Int
  IsZero :: Term Int -> Term Bool
  If     :: Term Bool -> Term a -> Term a -> Term a
```

Pattern matching refines types: matching on `Lit i` teaches the compiler `a ~ Int` in that branch. Makes illegal states un-representable at the expression level. `eval :: Term a -> a` is total — every branch returns exactly the right type.

### STM (Haskell)

`atomically` executes STM actions optimistically — reads/writes to TVars logged, validated at commit, silently re-run on conflict. `retry` blocks until any read variable changes. `orElse a b` tries a, falls back to b on retry.

Composability: two correct concurrent operations composed into a correct combined operation. Impossible with locks. Type system separates STM from IO — prevents side effects inside transactions.

### Deriving Via (Haskell)

Separates what a type is from how it behaves for a given interface. Derive an instance by treating the type as another type with identical representation. GHC.Generics provides uniform structural decomposition of any algebraic type into sums of products, enabling libraries to derive instances for any user-defined type automatically.

### OCaml Functors

Functions from modules to modules. Pass a module satisfying a signature, get back a new module specialized to that type. Each application creates genuinely new abstract types. Dependency injection, type minting, and sharing constraints at the module level.

### OCaml Polymorphic Variants

Structurally typed tags usable without prior declaration. `` `Error "not found" `` is valid in any type including the `` `Error `` tag. Type system infers the minimum set of tags a function can produce or consume. Solves the expression problem for error types — libraries compose without a unifying error type.

Tradeoff: lose exhaustiveness guarantees, error messages harder to read, less efficient codegen.

### OCaml 5 Effect Handlers

Algebraic effect handlers as first-class restartable exceptions. `perform` an effect, handler can `resume` the computation passing a value. Implemented on fibers — heap-allocated stack segments. Decouples what a computation does from how effects are interpreted.

Currently untyped in OCaml 5 — type system doesn't track which effects a function performs.

### Conditions and Restarts (Common Lisp)

Three-part error handling separating detection, mechanism, and policy:

- **Low-level code** detects problem and **signals** a condition.
- **Mid-level code** establishes **restarts** — named recovery strategies at the error point.
- **High-level code** establishes **handlers** that decide which restart to invoke.

`HANDLER-BIND` runs handlers without unwinding the stack. Handler executes in original context, examines the situation, invokes a restart. Recovery policy and mechanism separated. Library offers multiple strategies; application chooses without knowing library internals.

### CLOS Multiple Dispatch (Common Lisp)

Methods belong to no class. Define a generic function, then methods specialized on any combination of argument types. `(intersect rect ellipse)` dispatches on the full tuple. Method combination: `:before`, `:after`, `:around` methods modify execution flow. `:around` wraps the primary and can choose whether to call it.

Eliminates the Visitor pattern. Makes binary operations (collision detection, type coercion) natural.

### MOP (Common Lisp)

Classes, methods, generic functions, and slots are themselves first-class objects — instances of metaclasses. `standard-class` is an instance of `standard-class`. Every aspect of the object system is defined by methods you can override. Want persistent objects? Override slot access to read/write from a database. The language's mechanisms are implemented as overridable protocols.

### Koka Row-Polymorphic Effects

Every function type includes an effect row: `fun read() : <io,exn> string`. Effect rows are row-polymorphic: `fun map(f, xs) : <e> list<b>` works regardless of what effects `f` performs. No function coloring — effects compose transparently through row polymorphism.

### Effects vs Monads

Monads require transformer stacks with fixed ordering (does Error wrap State, or State wrap Error? — semantics change). Algebraic effects compose by union — order doesn't matter, adding/removing effects doesn't change existing code. Effects are "exceptions you can resume" — handler receives operation + continuation, can resume zero or more times.

### Unison Abilities

Every function type: `I ->{A} O` where A is the set of abilities. A function cannot sneak IO into a pure computation. Enables replayability, deterministic testing, compiler optimization (pure functions memoizable/parallelizable).

### Evidence Passing (Compiling Effects)

Instead of capturing continuations, handler evidence (record of handler functions) is threaded as an implicit parameter. Effects resolved by lookup at call site. Compiles to ordinary C function calls. No GC, no green threads, no stack manipulation. Competitive performance.

### APL Array Programming

The array is the only data structure. Scalars are rank-0, vectors rank-1, matrices rank-2. All operations defined on arrays and extend across dimensions. No loops — operations at the aggregate level. `+/ . x` is matrix multiplication. The mathematical structure is visible; the compiler chooses implementation.

### Rank Polymorphism (APL/J)

When a rank-r function is applied to a rank-n array (n > r), the system decomposes into rank-r cells within a rank-(n-r) frame, applies per cell, reassembles. Scalar function on matrix = element-wise. Vector function on matrix = row-wise.

One function definition works at every dimensionality. `lerp` on scalars automatically blends pixels (rank-1), adjusts image brightness (rank-2), creates video fades (rank-3). Iteration space derived from data, not specified by programmer.

### J Tacit Programming (Forks and Hooks)

**Fork** `(f g h) y` = `(f y) g (h y)`. Average: `(+/ % #)` — sum divided by count.
**Hook** `(f g) y` = `y f (g y)`. Subtract mean: `(- mean)`.

Universal combinators expressing any algorithm by composing primitives without naming intermediate variables. Data flow topology is explicit and visible. Programs already in a form amenable to data-flow analysis.

---

## Security / Capabilities / Verification

### Capability-Based Security

Authority conferred by possession of an unforgeable reference. Three rules (from E):
1. A sends to B only if A holds a reference to B
2. A obtains reference to C only by receiving a message containing it
3. References are unforgeable

Solves the confused deputy problem and eliminates ambient authority by construction. A module can only access what it was explicitly given.

### Austral's Capability + Linear Type Model

Root capability enters via `main`'s first argument. Narrower capabilities derive from broader ones. Capabilities are linear types — can't be duplicated or conjured from nothing.

Linearity checker is under 600 lines of OCaml. Each linear variable has a state: Unconsumed, BorrowedRead, BorrowedWrite, or Consumed. If/case branches must leave linear variables in the same state. Linear variables outside a loop cannot be consumed inside. All linear variables must be consumed before returning.

Philosophy: "Simple rules you can understand vs. complex heuristics you get used to."

### WASI Capability Security

Modules get zero capabilities by default. Host explicitly passes pre-opened file descriptors, network handles. No ambient authorities, no global namespaces, no global functions at link time. In production: Wasmtime, Fastly, Cloudflare Workers.

### Session Types

Communication protocols as types:

```
SessionType ::= send T; S | recv T; S | choose {L: S | R: S} | offer {L: S | R: S} | end
```

Duality ensures both sides agree. ATM example:

```
ATM = recv UserID; choose {
    Authorized: offer { Deposit: recv Amount; send Balance; recurse, ... },
    Rejected: send Error; end
}
```

In languages with move semantics, each operation consumes the old channel and returns a new one with updated type. Protocol violations are compile-time type errors.

Linearity is the largest barrier to adoption. Languages with ownership/move semantics get session types as a library, not a language feature.

### LiquidHaskell Refinement Types

Types augmented with logical predicates checked by SMT: `{x : Int | x > 0}`. Predicates in QF-EUFLIA (quantifier-free equality, uninterpreted functions, linear integer arithmetic) — decidable and efficiently checkable by Z3.

Verified 10,000+ lines of real Haskell libraries. Properties: array bounds safety, totality, termination, ordered data structures, memory safety (caught HeartBleed-class bugs).

Annotation burden: simple numeric bounds need minimal annotation, complex invariants need more. Type inference reduces burden for common cases.

### F* Verified Code in Production

HACL*: verified crypto library in Firefox, Linux kernel, Python, WireGuard. EverParse: verified binary parsers — every network packet in Azure/Hyper-V. StarMalloc: verified concurrent memory allocator.

Hybrid approach: dependent types + SMT automation + tactics. Simple proofs automated, complex ones guided manually.

### Idris 2 Quantitative Type Theory

Unifies dependent types with linearity through multiplicities: 0 (erased at runtime), 1 (used exactly once / linear), unrestricted (normal). State machines encoded directly in types with linear resource tracking:

```idris
openDoor : (1 d : Door Closed) -> Door Open
```

### Clean Uniqueness Types

Unique values guaranteed to have no aliases at runtime. Compiler safely performs destructive updates. I/O handled by threading a unique `World` value. Uniqueness inferred automatically.

### Futhark Practical Uniqueness Types

Distinguishes observation (reading) from consumption (mutation). Allows multiple reads before a single consuming update. `A with [i] = v` performs guaranteed in-place update. More practical than strict "use exactly once."

### Affine Types (Rust-style) vs Linear Types (Austral-style)

Affine: values can be dropped implicitly (going out of scope is fine). Works with exceptions, early returns, general ergonomics.

Linear: values MUST be consumed explicitly. Ensures critical cleanup always happens. No resource leaks even in error paths. Conceptually simpler but requires explicit destructor calls.

### Typed Strings via Regex (Bosque)

`typedecl CSSpt = /[0-9]+pt/;` then `StringOf<CSSpt>` in signatures. Regex-validated at construction, zero cost at runtime. Prevents injection attacks by making unvalidated strings a different type.

### Numeric Range Types

`typedecl Percentage = Nat & { invariant $value <= 100n; }`. Compiler knows the domain. Combines Ada's range types with SMT checking.

### Dafny Auto-Verification

`requires`, `ensures`, and `invariant` annotations dispatched to Z3. Catches: index out-of-bounds, null dereference, division by zero, infinite loops, logic errors. Compiles to C#, Java, Go, Python, JavaScript — verification coexists with practical codegen.

### TLA+ Concepts

Specification vs implementation as separate concerns. Refinement as logical implication (X implements Y means X => Y). Composition through conjunction (components impose constraints, composition intersects them). State + actions as the fundamental model.

---

## Cross-Cutting Themes

### Errdefer (Zig)

`defer` runs on scope exit. `errdefer` runs only on error exit. Write cleanup next to allocation:

```zig
var buf = alloc();
errdefer free(buf);
// if function returns error, buf is freed
// if function returns success, buf is NOT freed
```

### Design by Contract (Eiffel/Ada)

Pre/post-conditions as first-class syntax. In debug: runtime assertions. In release: compiler optimizes based on contracts. Precondition violation = caller's bug. Postcondition violation = function's bug.

### Structured Concurrency (Trio/Kotlin/Swift/Java 21)

All concurrent tasks live inside a scope that doesn't exit until all children complete. If any child fails, siblings are cancelled, error propagates to parent. No fire-and-forget. Task lifetimes follow lexical scope.

### Typestate

State machine transitions in the type system. A closed file is a different type from an open file. Calling `write()` on a closed file is a compile-time type error. Methods declare required state and resulting state.

### Region-Based Memory (Cyclone/ML Kit)

Allocate into named regions, free everything at once. O(1) deallocation for arbitrarily many objects. Type system ensures references don't escape their region. Natural for request-response, compiler passes, game frames.

### Multiple Dispatch (Julia/CLOS)

Select implementation based on ALL argument types, not just the first. Binary operations natural. Eliminates Visitor pattern. Static multiple dispatch (resolved at compile time) has zero runtime cost.

### Content-Addressed Compilation (Unison/Nix)

Artifacts keyed by hash of all inputs. Two builds with identical inputs produce identical outputs. Perfect incremental compilation. Near-instant rebuilds when few files changed.
