# Language Feature Research

Ideas stolen from the best programming languages ever designed, filtered for what ShelbyC should actually take. Organized by priority.

---

## Tier 1 — Should have, low complexity

### Errdefer (Zig)

`defer` runs cleanup code when a scope exits. `errdefer` runs cleanup code only on error exit. Write cleanup right next to allocation:

```
fd := Open(path)?;
errdefer Close(fd);    // only runs if function returns err
buf := Alloc(1024)?;
errdefer Free(buf);    // only runs if function returns err
// ... use fd and buf ...
return ok(result);     // neither errdefer runs
```

ShelbyC already has `defer`. Adding `errdefer` is a small parser/codegen change with massive usability impact. Handles the "undo on error" pattern that's everywhere in systems code.

**Source:** Zig

### Inline Tests

Test blocks live next to the code they test, compiled conditionally:

```
fn Divide(a i32, b i32) Result<i32, str> {
    if b == 0 { return err("division by zero"); }
    return ok(a / b);
}

test "divide happy path" {
    assert(Divide(10, 2) == ok(5));
}

test "divide by zero" {
    assert(Divide(10, 0).is_err());
}
```

No external test framework. The compiler includes or excludes test blocks based on `shelbyc test` vs `shelbyc build`. Tests that live with the code stay up to date.

**Source:** D, Rust

### Pipeline Operator Convention

ShelbyC already has `|>`. The missing piece is the convention: the "subject" is always the first parameter. If every stdlib function follows this, pipes work universally:

```
data |> Parse |> Validate |> Transform |> Serialize
```

Not a language change — a stdlib design principle. Document it, enforce it in the standard library, and the ecosystem follows.

**Source:** Elixir, F#

---

## Tier 2 — Should have, moderate complexity

### Design by Contract

Functions declare preconditions and postconditions. The compiler checks them:

```
fn Sqrt(x f64) f64
    requires x >= 0
    ensures result * result ~= x
{
    // ...
}
```

In debug builds: runtime assertions. In release builds: the compiler optimizes based on contracts (e.g., eliminating bounds checks when a precondition guarantees valid indices). Eventually: SMT solver proves contracts statically.

Contracts turn informal API docs into machine-checked specs. A precondition violation means the caller has a bug. A postcondition violation means the function has a bug. No ambiguity.

**Source:** Eiffel, Ada/SPARK

### Structured Concurrency

Every `wake` must live inside a scope that doesn't exit until all child wakes complete. If any child fails, siblings are cancelled and the error propagates:

```
nursery {
    wake { FetchData(url1) }
    wake { FetchData(url2) }
    wake { FetchData(url3) }
}
// all three are guaranteed complete (or cancelled) here
```

No fire-and-forget. Wake lifetimes follow lexical scope. For the rare case where you genuinely need detached wakes, an explicit `detach` keyword opts out.

ShelbyC already has `TaskScope` in the runtime. This makes it a language-level guarantee.

**Source:** Trio (Python), Kotlin, Swift, Java 21

### Supervision Trees

Supervisors monitor child wakes and restart them on failure:

```
supervisor RestartAll {
    wake { DatabaseConnection() }
    wake { CacheManager() }
    wake { RequestHandler() }
}
```

Restart strategies: one-for-one (restart just the failed wake), one-for-all (restart all siblings), rest-for-one (restart wakes started after the failed one). Intensity limits prevent restart storms.

Research shows ~131 of 132 production errors are transient — they disappear on restart. Structure the system so critical parts live near the root, fragile parts at the leaves.

**Source:** Erlang/OTP

### Typestate

Encode state machine transitions into the type system:

```
state Connection {
    Disconnected;
    Connected;
    Authenticated;
}

fn Connect(c Connection<Disconnected>) Connection<Connected> { ... }
fn Auth(c Connection<Connected>, creds Creds) Connection<Authenticated> { ... }
fn Send(c Connection<Authenticated>, data []u8) { ... }
```

Calling `Send` on a `Disconnected` connection is a compile-time type error. Combined with flow ownership, the compiler guarantees you can't use a connection in the wrong state.

Useful for: file handles, network protocols, hardware registers, driver state machines.

**Source:** Plaid, Obsidian

---

## Tier 3 — High value, significant complexity

### Refinement Types (Pragmatic Subset)

Augment types with logical predicates checked at compile time:

```
fn Divide(x i32, y i32 where y != 0) i32 {
    return x / y;
}

fn GetIndex(arr []T, i usize where i < arr.Len()) T {
    return arr[i];
}
```

An SMT solver (Z3) verifies these at compile time. Where the solver can't prove safety, it falls back to a runtime check with a clear error. Eliminates: division by zero, array out-of-bounds, integer overflow from invalid ranges.

More powerful than assertions, less complex than full dependent types. LiquidHaskell proved this approach works with low annotation burden — the solver infers most predicates automatically.

**Source:** LiquidHaskell, F*

### Conditions and Restarts

Superior error recovery that separates signaling, handling, and recovery:

```
fn ReadConfig(path str) Config {
    data := ReadFile(path)
        restart RetryWith(alt str) { return ReadFile(alt); }
        restart UseDefault() { return DefaultConfig(); };
    return Parse(data);
}

// Caller decides recovery policy
handle ReadConfig("config.json") {
    on FileNotFound => invoke RetryWith("/etc/default.conf")
    on ParseError => invoke UseDefault()
}
```

The code closest to the error defines what recoveries are possible. The code furthest away decides which recovery to use. The stack doesn't unwind until a recovery is chosen. Pairs with `Result<T, E>` — restarts are the "what do we do about it" layer on top of "what went wrong."

**Source:** Common Lisp

### Capability-Based Security

All system access requires unforgeable capability tokens:

```
fn Main(root RootCapability) {
    fs := root.FileSystem();
    net := root.Network();

    // Libraries only get what they need
    server := StartServer(net.Listen(8080));
    config := LoadConfig(fs.Open("config.json"));

    // A logging library can't access the network
    // because it never receives a Network capability
    InitLogger(fs.OpenDir("/var/log"));
}
```

No global `Open()`, no ambient authority. Libraries declare what capabilities they need in their signatures. The call graph makes privilege flow visible and auditable. Combined with flow ownership (capabilities can't be duplicated without explicit clone), this creates a verifiable security boundary.

**Source:** E language, Austral, WASI

### Algebraic Effects (Pragmatic Subset)

Effects generalize exceptions so that a function can "perform" an effect and a handler decides what happens — crucially, the handler can resume execution:

```
effect Log {
    fn Write(msg str);
}

effect Async {
    fn Await<T>(future Future<T>) T;
}

fn ProcessData(data []u8) performs Log, Async {
    Log.Write("starting");
    result := Async.Await(Transform(data));
    Log.Write("done");
    return result;
}
```

The same code runs synchronously in tests (mock handler) and asynchronously in production (real handler). Solves the "function coloring" problem — `async` doesn't infect all callers. Subsumes exceptions, coroutines, generators, and async/await into one mechanism.

Don't do the full Koka effect system. Start with a fixed set of built-in effects (IO, Fail, Async) and add user-defined effects later.

**Source:** Koka, Eff, OCaml 5, Unison

---

## Tier 4 — Worth considering, niche value

### Pony's Transition Capability

A value starts as uniquely mutable (`unique`), and once construction is complete, transitions to globally immutable (`frozen`), safely shareable across wakes without copying:

```
builder := unique JsonDoc.New();
builder.AddField("name", "ShelbyC");
builder.AddField("version", "0.11");
doc := freeze(builder);  // transitions to immutable, shareable
// builder is now consumed — can't use it
wake { ProcessDoc(doc); }  // safe: doc is frozen
wake { PrintDoc(doc); }    // safe: doc is frozen
```

This captures the builder pattern at the type level. More intuitive than Pony's six capabilities but captures the most valuable pattern.

**Source:** Pony

### Region-Based Memory / Explicit Arenas

Allocate into named regions, free everything at once:

```
region r {
    nodes := r.Alloc(Vec<Node>.New());
    edges := r.Alloc(Vec<Edge>.New());
    BuildGraph(nodes, edges);
    result := Analyze(nodes, edges);
}
// everything in r freed here, O(1) deallocation
```

The type system ensures references to region-allocated data don't escape the region. Natural for request-response patterns, compiler passes, game frames. PostgreSQL uses this pattern pervasively ("memory contexts").

ShelbyC already has `arena` as a reserved keyword. This is the design.

**Source:** Cyclone, ML Kit, PostgreSQL

### Multiple Dispatch (Static)

Resolve function overloads based on ALL argument types at compile time:

```
fn Add(a i32, b i32) i32 { return a + b; }
fn Add(a f64, b f64) f64 { return a + b; }
fn Add(a Vec3, b Vec3) Vec3 { ... }
fn Add(a Matrix, b Vec3) Vec3 { ... }
```

Julia does this dynamically. ShelbyC can do it statically via monomorphization — zero runtime cost, full expressiveness. Eliminates the expression problem (adding new types AND new operations).

**Source:** Julia

### Mutable Value Semantics — Parameter Modes

Make function parameter access explicit:

```
fn Sort(inout arr []T) { ... }      // modifies in place
fn Count(let arr []T) i32 { ... }   // read only
fn Consume(sink arr []T) { ... }    // takes ownership
fn Init(set out T) { ... }          // initializes uninitialized
```

Every function's access to data is visible in its signature. Combined with flow ownership, this makes code self-documenting and enables compiler optimizations.

**Source:** Hylo, Swift (inout)

### Content-Addressed Compilation Cache

Compilation artifacts keyed by `hash(source + deps + flags)`. Two builds with identical inputs produce identical outputs:

```
$ shelbyc build src/     # first build: 12 seconds
$ touch src/lexer.smc
$ shelbyc build src/     # only recompiles lexer.smc: 0.3 seconds
$ git stash && git stash pop
$ shelbyc build src/     # cache hit on everything: 0.01 seconds
```

Not a language feature — a compiler infrastructure decision. Build it in from day one.

**Source:** Unison, Nix, Zig

### Compile-Time Architecture Constraints (Datalog-inspired)

Express module-level invariants as rules:

```
@constraint no_import("drivers", "userspace")
@constraint all_fields_serialize(Packet)
@constraint max_complexity(20)
```

The compiler checks these against the actual code. Architecture enforcement in the type system, not in a wiki page nobody reads.

**Source:** Datalog, internal tools at Google/Meta

---

## Already Covered by ShelbyC's Design

These features from other languages are already handled:

| Feature | Language | ShelbyC equivalent |
|---------|----------|--------------------|
| Borrow checker | Rust | Flow ownership (no annotations) |
| Goroutines | Go | Wakes |
| Channels | Go | Chan<T> (planned) |
| Pattern matching | Haskell, Rust | `match` with destructuring |
| Option/Result | Rust, Haskell | Option<T>, Result<T,E>, `?` |
| Generics | Most modern | Monomorphization |
| Interfaces | Go | Implicit satisfaction |
| Compile-time eval | Zig, D | Comptime (Phase 8) |
| Type reflection | C++26, D | T.Fields(), T.Methods() (Phase 8) |
| Zero-cost abstractions | Rust, C++ | LLVM backend |
| Green threads | Go, Erlang | Wakes with M:N scheduler |
| Closures | Most modern | Full captures, deferred emission |
| Defer | Go | Already implemented |
| Pipeline operator | Elixir, F# | `|>` already in parser |
| Balanced ternary | Nobody | trit/tryte (novel) |
| Self-hosting semantics | Nobody | Flow ownership on itself (novel) |
