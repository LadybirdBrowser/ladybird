# The LibJS optimizing JIT

This document describes how the optimizing JIT works. It is a living document: every change to the
compiler's structure updates it, and the code reads like it. Where it and the code disagree, one of
them has a bug.

The compiler is the `libjs_jit` crate (`Libraries/LibJS/JIT/Rust`). Its runtime side, which takes
snapshots, installs code, enters it and translates its state back into interpreter frames, is in
`Libraries/LibJS/Rust/src/jit`.

NB: The runtime side does not have code that depends on things staying as they are (`jit/dependencies.rs`)
yet. Compiled code checks what it relies on until it arrives in a later change.

## 1. Overview

LibJS has two tiers: the bytecode interpreter, whose instructions keep per-site caches and
feedback, and this optimizing JIT. An executable that uses up its tier-up budget (calls and loop
iterations) is snapshotted and compiled on the compile thread. The interpreter keeps running it
meanwhile; the main thread installs the code when the job finishes. Loops that run long enough
enter compiled code at their back edges (on-stack replacement).

There is no baseline tier. The interpreter's caches are the baseline: the JIT translates what they
saw into speculation, and leaves to the interpreter wherever a speculation fails.

## 2. Phases

| Phase | Where | What it does |
|---|---|---|
| Snapshot | `jit/snapshot.rs`, `snapshot.rs` | Captures the executable, its inlining candidates, their caches and feedback, and frame layouts as plain data. The compiler never sees the heap. |
| Graph building | `builder/` | One forward walk over the bytecode in reverse post order builds SSA IR, translating caches and feedback into checks and typed nodes, inlining calls, and attaching frame states. It folds only locally, like a check of a value it knows or the unboxing of a box it just made. |
| Passes | `passes/` | Simplify the graph, one transformation per pass (section 6). The verifier checks the graph after building and after every pass. |
| Register allocation | `regalloc/` | One forward walk over the blocks in linear order, driven by the constraints each node declares. Its own verifier simulates every location. |
| Code generation | `codegen/` | Lowers each node onto a `PortableMacroAssembler`, emits sites (section 4) and the stubs that reach the runtime. |
| Install | `jit/tier_up.rs`, `jit/code.rs`, `jit/dependencies.rs` | Installs the code if every dependency still holds (section 5). |
| Run | `jit/entry_exit.rs`, `jit/translate.rs`, `jit/calls.rs` | Enters the code; leaves it at exits, leaves and calls through the translator. |

## 3. The IR

A `Graph` is a list of blocks in linear order. Every block has phis, a body of nodes and exactly
one control node. Nodes live in one arena and are named by `NodeId`. Constants belong to no block
and are rematerialized at every use. Every value has a representation (`Repr`): tagged, int32,
float64, bool or pointer. Cold blocks hold code that rarely runs (slow paths of checks); they come last
in the final block order.

Blocks without predecessors are entries: block 0 and the on-stack replacement entries. Every block
is reachable from one.

The compiled function's slots are SSA values. The builder tracks, per slot, whether only frame
memory holds it or which value holds it (and whether memory holds that too). Merges and loop
headers never write frame memory: a slot that every path leaves in memory stays there, a slot the
paths disagree on (or a loop assigns) gets a phi, with loads at the predecessors that only have it
in memory. Frame memory is written only where something reads it: before calls that read the
frame (`StoreSlot`), and by exits and leaves through the translator. The this value register is
the exception at loop headers: slow paths write it to memory, so loops keep it there.

### 3.1 Effects

`Op::properties()` describes every op's effects, and every pass decides what it may do with a node
from them alone:

- **Exits.** `Op::can_eager_exit()`, whether the op has an exit kind (`Op::exit_kind()`): may exit
  before its side effects, resuming at the current bytecode (needs the eager frame state).
  `can_lazy_exit`: may need to exit after it returns, resuming after the current bytecode.
- **Call.** `is_call`: calls out of compiled code and clobbers every caller-saved register.
- **Memory.** `reads` and `writes`: sets of abstract locations (`ir::Locations`): `FRAME`,
  `NAMED_SLOTS` (property values), `SHAPES` (shapes and prototypes), `ELEMENTS` (element values),
  `ELEMENT_COUNTS` (element storage kind, length and capacity), `GLOBAL_BINDINGS` and `BINDINGS` (the
  bindings of declarative environments). Passes ask
  one question: whether one node's writes intersect another's reads. `runs_code`: may run other
  code, which may write anything, or invalidate the compiled code. The op table below names the
  locations coarsely (Named for slots and shapes, Elements for values and counts, Any for all);
  `Op::properties()` has the exact sets.
- **Allocation.** `allocates`: may allocate, so may run the garbage collector.

Global value numbering shares nodes without effects that read no memory; check elimination reuses
loads.

### 3.2 Refinements

A node that relies on a check takes the check's value as its input, never the checked value
itself. Checks that exit (`CheckObject`, `CheckShape`, `CheckValue`, `CheckClosure`,
`CheckAccessorFunction`, `CheckElements`, `CheckIdentityComparable`, `CheckAppendableArray`,
`CheckBounds`, `CheckNotHole`) have their input 0 as their value, refined: known to pass.
`CheckInt32` refines and unboxes at once. A branch refines on its edges with `Refine`, the
textbook pi node: it takes the branch's inputs and returns input 0, at the head of the block that
only that edge reaches. So every check is a data dependency of what relies on it: passes move or
share a load exactly when they can move or share its check, without control dependencies and
without pinning loads by making them read everything.

Passes see through refinements where that is sound: check elimination keeps its facts about the
unrefined value, and leaves the uses of a check it removes on the value the check refined.
Global value numbering does not look through them, since a node shared with another of a weaker
refinement would lose its dependency. After the last optimization pass, every use of a refined
value becomes a use of the value itself (`edit::remove_refinements()`): `Refine` nodes go away,
checks lose their values, and a final round of value numbering merges the nodes that differed only
in their refinements. Register allocation and code generation never see a refinement.

### 3.3 Op reference

Exits: E = eager, L = lazy. Every node that can exit has a frame state. "Call" means `is_call`.
`cse` marks ops two of which with the same inputs compute the same value.

| Op | Semantics | Exits | Reads | Writes | Other |
|---|---|---|---|---|---|
| **Values** | | | | | |
| Constant | A NaN-boxed constant. | | | | cse |
| Phi | One input per predecessor, in predecessor order. | | | | |
| LoadSlot | Reads a frame slot. | | Frame | | |
| StoreSlot | Writes input 0 to a frame slot: a reserved register (the this value, the saved lexical environment), a written constant, or a slot the next call reading the frame reads. The verifier enforces this. | | | Frame | |
| BoxInt32, BoxBool | The tagged value of an unboxed input. | | | | cse |
| CheckInt32 | Input 0 as an int32; exits unless it is one. | E | | | cse |
| CheckNumber | Input 0 as a float64; exits unless it is a number. | E | | | cse |
| EmptyToUndefined | Input 0 with the empty value replaced by `undefined`. | | | | cse |
| CellAddress | The cell pointer of a tagged value, shared by the nodes accessing the object. | | | | cse |
| UnboxInt32, UnboxDouble | The int32 or float64 of a tagged value that a branch established is an int32 or a double. | | | | cse |
| BoxFloat64 | The tagged number of a float64 (an int32 where it is one, NaN canonicalized). | | | | cse |
| StringAddress | The cell pointer of a string. | | | | cse |
| **Arithmetic and tests** | | | | | |
| Int32Binary | Int32 arithmetic; exits where the result is no int32 (overflow, -0, a zero divisor). | E if it can overflow | | | cse |
| Uint32ShiftRight, Uint32ToFloat64 | `>>>` of int32 values as the bits of the unsigned result, and the float64 of such bits. | | | | cse |
| Int32Compare | Int32 comparison as a bool. | | | | cse |
| TaggedEquals | Bit equality of tagged values, for values where that is strict equality. | | | | cse |
| Int32Abs, Int32ToFloat64, Float64Unary | Int32 absolute value, conversion to float64, negation and `Math` operations on float64. | | | | cse |
| Float64Binary, Float64Compare, Float64ToInt32 | Float64 arithmetic, comparison as a bool (false for NaN but in inequalities), and ToInt32. | | | | cse |
| IsCallable | Whether input 0 is a function object. | | | | cse |
| Typeof, TypeofIs | `typeof` of input 0, or whether it is one kind. | | | | cse |
| ToBoolean | Truthiness through the runtime's `to_boolean`; the slow path of `BranchTruthy`. | | | | call, cse |
| PrimitiveToString, ToObject | ToString of a primitive, and ToObject of a primitive (but undefined and null) as a builtin of the running realm makes it (else the empty value), through the runtime. | | | | call, allocates |
| ArrayCreate | `new Array(length)` as the running realm's `Array` constructor makes it, through the runtime. | | | | call, allocates |
| **Checks** | | | | | |
| CheckObject, CheckValue, CheckClosure, CheckIdentityComparable | Exit unless input 0 is an object, a given value, a closure of a given function, or identity comparable. The value is input 0, refined (section 3.2). | E | | | cse |
| CheckShape | Exits unless the object has one of the shapes. The value is the object, refined. | E | Named | | |
| CheckPrototypeChainValid | Exits unless a prototype chain validity cell is still valid. | E | Named | | |
| CheckAccessorFunction | Exits unless an accessor property holds a given getter or setter. The value is the object, refined. | E | Named | | |
| CheckElements | Exits unless the object has elements of a kind. The value is the object, refined. | E | Elements | | |
| Refine | Input 0, refined by the edge of a branch on the same condition and inputs: at the head of the block only that edge reaches (section 3.2). Never moved; gone before register allocation. | | | | cse |
| CheckBounds | Exits unless the index (input 0) is below the count (input 1), unsigned. The value is the index, refined. | E | | | cse |
| CheckNotHole | Exits at holes (the empty value). The value is input 0, refined. | E | | | cse |
| AssumeValid | Exits if the code was invalidated; a patchable no-op (section 5). | E | Any | | |
| **Object memory** | | | | | |
| LoadNamed | A named property at an offset; exits for accessors. | E | Named | | cse |
| StoreNamed | Stores an existing named property; exits for accessors. | E | Named | Named | |
| AddNamed | Adds a property to a fresh object and gives it the new shape. | | Named | Named | |
| LoadAccessorFunction | The getter or setter of an accessor property. | E | Named | | |
| HasInPrototypeChain | Whether input 1 is on input 0's prototype chain (the OrdinaryHasInstance loop). | E | Named | | |
| LoadElementAt, StoreElementAt | An element of a known elements kind at an index in bounds, without checks; holes load as the empty value. Typed arrays load int32 or float64 and store int32 (integer kinds) or float64. | | (Load) Elements | (Store) Elements | |
| LoadTypedArrayLength | The length of a typed array, or 0 while its data is not cached (detached or resizable buffers). | | Elements | | |
| CheckAppendableArray | Exits unless `push` can append to the array without observable steps. The value is the array, refined. | E | Any | | |
| LoadElementsLength, LoadElementsCapacity | The array-like size, and the room of packed or holey storage (only corruption makes the size larger). | | Elements | | |
| ProbePropertyCache | The data property (own or of an unchanged prototype chain), or `undefined` for a missing one, that the instruction's property lookup cache has for the object, like `GetById` reads it; the empty value if none. The runtime probes the other entries out of line. One atomic read of a cache the interpreter keeps changing. | | Named | | saves GPRs |
| ProbeKeyedCache | The own data property, or `undefined` for a missing one, that the keyed cache or the VM's keyed cache has for the object and key; the empty value if none. | | Named | | |
| ProbePropertyStore, ProbeKeyedStore | Stores (or adds) the property through such an entry, like `PutById` and `PutByValue`, as a bool telling whether it did. The named one also probes through the runtime, outside inlined callees. | | Named | Named | allocates, saves GPRs |
| ProbeGlobalCache, ProbeGlobalStore | The global variable a global variable cache is for (a data property of the global object of the cached shape, or an initialized binding of the global declarative environment), like `GetGlobal` and `SetGlobal`; the empty value, or false, where the cache does not apply. Inputs: the realm and the executable (and for stores, the value first). | | Any | (Store) Any | |
| ProbeHasProperty | Whether the object has the property the key names, as its own (`hasOwnProperty`) or at all (`in`), where the VM can tell without running code: from the VM's keyed cache and, for `in`, the elements inline, and from the runtime's lookup out of line; the empty value otherwise. | | Any | | saves GPRs |
| AppendElement | Appends an element below the capacity. | | Elements | Elements | |
| CallArrayPush | Appends an element, growing the elements, preserving every register. | | Elements | Elements | allocates |
| **Strings and for-in** | | | | | |
| StringLength, LoadStringCodeUnit | The length of a string, and a code unit of a readable string at an index in bounds, which the refinements of their branches establish. Strings never change. | | | | cse |
| SingleCharacterString | The VM's string of an ASCII code unit (refined below 128). | | | | cse |
| StringsEqual | Whether two strings are equal, as a tagged boolean, or the empty value where only the slow path can tell. | | | | cse |
| ConcatenateStrings | The concatenation of two strings, a rope or a cached short string, or the empty value where only the slow path makes it. | | | | allocates |
| IntegerToString | The VM's cached string of an int32, or the empty value. | | | | cse |
| LoadPropertyIteratorKeyCount, LoadPropertyIteratorKey | The keys of a property iterator cache, at an index refined to be in bounds. The keys never change. | | | | cse |
| LoadGlobalBinding | A binding of the global declarative environment; exits while uninitialized. | E | Global | | cse |
| StoreGlobalBinding | Stores a mutable global binding; exits while uninitialized. | E | Global | Global | |
| **Allocation** | | | | | |
| AllocateObject, AllocateArray | A new plain object or packed array; nothing can see it before it is stored. | | | | allocates |
| AllocateFunction, AllocateEnvironment | A new closure or lexical environment from its template. | | | | allocates |
| InitializeNamed, InitializeElement | Initializing stores into an object or array nothing else has seen. | | | Named / Elements | |
| VirtualObject | An object escape analysis removed, as frame states need it; in no block, no code. | | | | |
| **Frames and environments** | | | | | |
| InitializeFrame, EnsureFrameInitialized | The `Enter` bytecode: publishes the frame, empties the registers and locals, copies the constants (the second only if the frame is not initialized yet). | | | Frame | |
| PublishFrame | Makes the frame the running execution context, which direct calls leave to their callee's code. | | | Frame | |
| LoadFrameField | A field of the running execution context (an environment, the realm, the executable) as a pointer. The builder loads each once per path, until an instruction may replace it. | | Frame | | |
| SetLexicalEnvironment | Makes a cell value the frame's lexical environment; its value is the environment's pointer. | | Frame | Frame | |
| LeavePrivateEnvironment | Makes the private environment's outer one the frame's. | | Frame | Frame | |
| BoxCell | The tagged cell value of a pointer to an environment, as registers hold it. | | | | cse |
| LoadFunctionEnvironment | A closure's lexical or private environment as a pointer, fixed for its lifetime. | | | | cse |
| LoadOuterEnvironment | The outer environment of an environment, which never changes. | | | | cse |
| LoadEnvironmentBinding | A binding of a declarative environment, or the empty value while it is uninitialized. | | Bindings | | |
| StoreEnvironmentBinding | Stores a binding of a declarative environment. | | | Bindings | |
| AppendEnvironmentBinding | Appends the uninitialized binding a declarative environment's shape has next, where a branch on `NextBindingOfShape` found room. | | | Bindings | |
| ArgumentCount | The running frame's passed argument count. | | Frame | | cse |
| LoadArgument | `arguments[i]` of an arguments object never created; exits out of range. | E | Frame | | cse |
| SliceArguments | `Array.prototype.slice.call(arguments, start)` of an arguments object never created. | E | Frame | | call, allocates |
| **Calls** | | | | | |
| CallDirect | A `Call` to its feedback's ECMAScript target, entering its code directly. | L, throws | Any | Any | call, allocates |
| CallNative | A `Call` to its feedback's native target, in the interpreter's lightweight frame. | L, throws | Any | Any | call, allocates |
| CallForwardingArguments | `f.apply(this, arguments)` with an arguments object never created. | L, throws | Any | Any | call, allocates |
| CallSlowPath | One bytecode instruction through the interpreter's slow path, on SSA operands, with SSA outputs, with a frame state after the instruction. `saves_registers`: in a cold block, preserving every register. | L unless it saves registers; throws per opcode | Any | Any | call unless it saves registers; allocates |
| SlowPathOutput | A further output of the `CallSlowPath` right before it, which is its input (the verifier checks that it follows it). | | | | |
| Generic | One instruction through its slow path, on frame memory only. Left for instructions that read the whole frame (`eval`, `CreateArguments`, ...) and reads of an arguments object never created. | L, throws per opcode | Any | Any | call, allocates |
| **Control** | | | | | |
| Jump, Branch, BranchTruthy, BranchOnPc | Unconditional and conditional control flow. `Branch` conditions include `ElementsKind` (as `CheckElements` checks), `IndexInBounds` (as `CheckBounds` checks), `MagicalLength`, `Extensible`, `BindingMutable` (whether a binding of a declarative environment is mutable, which never changes) and `NextBindingOfShape` (whether an environment can append the binding its shape has next). Code generation fuses a probe with the branch on its result where the result is not live into the branch's target for failure. | | | | |
| ShapeSwitch | Continues at the case of the object's shape; exits for other shapes. Each case starts with a `Refine` on `Shape` of the object. | E | Named | | |
| Return | Returns input 0. | | | | |
| Exit | Exits to the interpreter unconditionally. | E | | | |
| Unreachable | Control never reaches the end of the block. | | | | |

### 3.4 Representations

To be written: tagged values, unboxed int32, float64 and bool, pointers, where boxing happens
(`passes/representation.rs`), and what frame states accept.

## 4. Frame states, sites and the translator

Compiled code keeps the interpreter's state in SSA values. Wherever the runtime may need that
state as interpreter frames, the IR carries a **frame state**: the bytecode position (`executable`,
`pc`), how to resume there (`ResumeAt` the instruction, or `ResumeAfter` it with the result in a
destination slot), and every live slot of the frame, as a value or as held by the frame
(`in_frame`). Frame states of inlined calls chain to the frame state of their caller.

Rules (enforced by `passes/verify.rs`):

- every node that can exit, eagerly or lazily, has a frame state;
- every live slot is listed exactly once, so the translator can clear every unlisted register and
  local;
- a `ResumeAfter` destination may be listed with its value from before the instruction (handlers
  of a throw read it); exits never write it, since the interpreter resumes with the result there.

Code generation turns each point where the runtime may read the state into a **site**
(`code::Site`): its kind, its chain of frame states with the location of every value (register,
stack slot, constant, an arguments object or a virtual object to create), and the virtual objects.

| Site kind | Where | What the runtime does |
|---|---|---|
| `Exit(reason)` | exits | Writes the frames (Full mode), records the reason so recompiles do not repeat the speculation, and resumes in the interpreter. |
| `Leave` | a slow path or call that did not continue in compiled code (a throw, a return through the interpreter) | Writes the frames that are still on the stack; nothing is counted. |
| `Publish` | a slow path or call in an inlined callee that runs in the inlined frames, where compiled code cannot write a header value itself | Pushes the inlined frames with their headers only (Header mode: function, `this`, arguments, pc), uninitialized; the slow path runs in the innermost one, and compiled code pops them if it continues. Compiled code publishes the same headers inline everywhere else. |
| `Call` | a call in an inlined callee, which runs without the inlined frames | Describes the inlined frames for stack walks (View mode); materializes them for the call's slow paths (Full mode). |

The **translator** (`jit/translate.rs`) is the only code that turns compiled state into
interpreter state. Full mode writes interpreter frames; Header mode pushes the frames of inlined
calls with their headers for slow paths and calls that take their operands as values; View mode
describes inlined frames without writing them.

**Publication.** A direct call builds its callee's frame without making it the running execution
context. Until the callee's code publishes it (`PublishFrame`, or `InitializeFrame` on the first
node that observes its slots), the frame is invisible: the garbage collector, stack walks and
runtime functions only see the running frame and its callers, so nothing may run other code,
collect garbage or exit lazily before that. Eager exits publish the frame in the translator, and
the prologue's resumption in the interpreter publishes it inline.


## 5. Dependencies and invalidation

Compiled code may assume facts that hold until a rare runtime event (`code::Dependency`): a
prototype chain stays valid, a shape stays stable, a global binding or a property of the global
object is never assigned, the global declarative environment gets no bindings, no `[[IsHTMLDDA]]`
object exists.

- The builder and check elimination record the dependencies they rely on in
  `Graph::dependencies`.
- The runtime installs code only if all of them still hold (`jit/dependencies.rs`), which also
  covers what changed while it compiled.
- Each kind of dependency stops holding at exactly one place in the runtime, the event every fast
  path writing the watched state needs first. That place calls `invalidate_dependents()`: the code
  is discarded, so nothing enters it anymore, and invalidated, so frames still running it exit
  where they next rely on it.
- Only code that runs while compiled code is suspended in a call or slow path can invalidate it.
  The first node after such a call that relies on a dependency is an `AssumeValid`: a no-op the
  runtime patches into a jump to its exit (`CompiledCode::invalidation_patches`).
- Properties of the global object (`GlobalPropertyUnassigned`) are written by the interpreter's
  caches and by compiled stores without the runtime, so their dependency rests on one rule: the
  runtime caches no write to a slot of a global object that is not assigned yet. The runtime's
  write path (`Object::storage_set()` and `storage_delete()`) is then the only writer of such a
  slot. It notes the slot as assigned when it replaces a value other than undefined with another
  one, or moves the property by deleting it or one before it, and invalidates the code depending
  on the slot. Initializing a slot (a `var` initializer, a lazily created intrinsic) or storing the
  value it holds (CreateGlobalFunctionBinding) is no assignment. Code only depends on this while
  the global object's shape is a dictionary: no other object gets that shape or one it leads to,
  so caches filled by writes to other objects never apply to the global object.

## 6. Passes

`passes::optimize()` runs the passes in this order, after removing the blocks graph building left
unreachable. Each one keeps the graph valid (`passes/verify.rs`), and IR tests in
`Tests/LibJS/JIT/ir` check their results.

| Pass | Contract |
|---|---|
| Trivial phi removal | Phis whose inputs are all one value (or the phi itself) become that value. |
| Simplification | Constant folding and strength reduction; branches on constants become jumps; unboxing a box is the boxed value. |
| Representation selection | Phis of int32, bool or float64 values become unboxed phis, and int32 inputs of float64 phis are converted. Tagged inputs of an int32 phi that the code checks on every pass are checked where they enter, exiting to the start of the phi's block. |
| Simplification | Again, for what unboxing made foldable. |
| Check elimination | A forward dataflow of facts removes checks that passed already and reuses loads whose value is known; may turn shape checks into `AssumeValid` plus a stable shape dependency. |
| Global value numbering | A CSE-able node computing what a dominating node computed is replaced by it. Memory reads are left to check elimination. |
| Loop invariant code motion | Gives loops entered from several blocks a merged preheader, then moves effect-free nodes with inputs from outside the loop to it; nodes that may exit only from blocks every iteration runs, exiting to the loop header, and not where that kind of exit was taken there before. |
| Dead code elimination | Removes nodes without uses and without effects; frame states count as uses. Branches whose edges lead to the same place doing the same thing become jumps. |
| Escape analysis | Allocations that do not escape become their properties' SSA values; frame states refer to `VirtualObject`s. |
| Dead code elimination | Again, for what escape analysis left unused. |
| Branch fusion | A branch on a single-use comparison (int32, float64 or tagged) compares itself, after a block that only branches on a phi of booleans hands the branch to its predecessors. |
| Refinement removal | Every use of a refined value becomes a use of the value itself; `Refine` nodes go away and checks lose their values; value numbering then merges what differed only in refinements (section 3.2). |
| Frame initialization | Moves `InitializeFrame` to the first node that observes the frame's slots on each path, and puts `PublishFrame` before nodes that only need the frame visible (allocations, calls in inlined callees) where it may not be yet. |
| Frame state cleanup | Drops the frame states no node uses. |
| Cold block layout | Moves the cold blocks after all others. |

## 7. Register allocation

To be written: the forward walk, node constraints, spills at definition, merges and their state
predecessor, cold blocks, the verifier.

## 8. Code generation and the runtime interface

To be written: per-op lowering, slow path calling conventions and the operand record, the call
stub, cache probes, the allocation contract, the entry and exit stubs.

## 9. Speculation and exits

To be written: exit reasons, recompiles, the rule that a failed speculation is never repeated.

## 10. The snapshot and the caches

To be written: what a snapshot captures, the thread rules, the one cache entry form
(`builder/caches.rs`) and its three consumers.

## 11. Testing

- Unit tests: `ninja libjs_jit-test`, per module (`builder/tests.rs`, `passes/tests.rs`, ...).
- IR tests: `Tests/LibJS/JIT/ir`, run by `Tests/LibJS/JIT/irtest.py`.
- `Tests/LibJS/JIT/matrix.py` runs test-js and the programs under every JIT mode and compares the
  results with the interpreter's.
- `Tests/LibJS/JIT/fuzz.py` generates programs and compares the JIT with the interpreter.
- Stress options (`LIBJS_JIT=on,...`, see 13.2): `stress-exits`, `stress-osr`, `stress-registers`,
  `stress-install`, `stress-invalidate`, `random-thresholds`; `verify-ir` checks the graph after
  every pass in release builds.

## 12. Where the ideas come from

To be written: how this design relates to the textbook designs of optimizing compilers.

## 13. Configuration

How to build without the JIT, and how to turn it off and tune it at run time.

### 13.1 Building without the JIT

The `ENABLE_JS_JIT` CMake option builds the JIT. It is on by default on x86-64 and AArch64 Linux and
macOS, the platforms the JIT generates code for, and off elsewhere, Windows included. To build
without it:

```bash
cmake --preset Release -DENABLE_JS_JIT=OFF
```

The option maps to the default `jit` feature of the `libjs_rust` crate. Without it, the `libjs_jit`
compiler crate is neither built nor linked, only the plain build of the interpreter is assembled
(not the profiling one, which collects feedback and counts tier-up budgets), and
`Libraries/LibJS/Rust/src/jit/disabled.rs` stands in for the runtime's JIT module. Such builds leave
out the JIT's tests (`test-js-jit`, `test-jit-ir`, `libjs_jit-test`, and the runtime tests in `jit`
directories, which use the `jit` object), and ignore `LIBJS_JIT` with a warning if it asks for the
JIT.

### 13.2 The LIBJS_JIT environment variable

Builds with the JIT run it only if the `LIBJS_JIT` environment variable turns it on. LibJS reads
it whenever it creates a VM, in every process that runs JavaScript: `js`, `test-js`,
`test262-runner`, and the browser's WebContent and WebWorker processes, to which the browser passes
on every `LIBJS_` variable.

`LIBJS_JIT` holds comma-separated options, applied left to right from the defaults. A value with an
unknown option, or with an option written wrong, stops the process with a message that says why.
`LIBJS_JIT=help` lists the options and the values in effect, which also works together with other
options:

```bash
LIBJS_JIT=on ./Build/release/bin/js script.js
LIBJS_JIT=threshold=100,warmup=2 ./Build/release/bin/js script.js
LIBJS_JIT=threshold=100,help ./Build/release/bin/js script.js
```

| Option                  | Default | What it does                                                                                                       |
|-------------------------|---------|--------------------------------------------------------------------------------------------------------------------|
| `on`                    |         | Compile hot functions.                                                                                             |
| `off`                   | off     | Run everything in the interpreter, which then collects no feedback.                                                |
| `help`                  |         | List the options and the values in effect on stderr.                                                               |
| `threshold=N`           | 400     | How many invocations make a function hot, after its warm-up. Loop iterations count as a fraction of an invocation. |
| `warmup=N`              | 8       | How many invocations a function runs before the interpreter collects feedback for it.                              |
| `sync`                  | off     | Compile on the main thread when a function gets hot, at the same points in every run, instead of on a compile thread. |
| `inline-budget=N`       | 250     | How many bytecode instructions the inlined callees of one compile may have together.                               |
| `inline-max-size=N`     | 30      | How many bytecode instructions one inlined callee may have.                                                        |
| `inline-depth=N`        | 5       | How deeply inlined calls may nest; 0 turns inlining off.                                                           |

The thresholds are the knobs for tuning: `threshold=0,warmup=0` compiles every function at its first
call (without feedback), and larger values compile less and later.

Options for debugging the JIT:

| Option               | What it does                                                                                    |
|----------------------|-------------------------------------------------------------------------------------------------|
| `dump-ir`            | Print the IR of every compile.                                                                  |
| `dump-passes`        | Print the IR after building it and after every pass.                                            |
| `dump-asm`           | Print the machine code of every compile.                                                        |
| `dump-feedback`      | Print the interpreter feedback of each function the first time it gets hot.                     |
| `verify-ir`          | Check the invariants of the IR after building it and after every pass, also in release builds.  |
| `log-exits`          | Print every exit from compiled code to the interpreter, and every discard of compiled code.     |
| `perf-map`           | Describe compiled code in `/tmp/perf-<pid>.map` for `perf`.                                     |
| `coverage=DIRECTORY` | Write what compiled code contained and did into `DIRECTORY`, one JSON file per VM.              |

Stress options for testing, which make compiled code take its rarely taken paths. With `sync` and
the same `seed`, a run makes the same random choices every time.

| Option                  | What it does                                                                                                   |
|-------------------------|----------------------------------------------------------------------------------------------------------------|
| `seed=N`                | Seed the random choices of the stress options (1 by default).                                                  |
| `stress-exits[=N]`      | Exit from compiled code at a random one of every N checks (20 if bare); 0 never does.                          |
| `stress-osr`            | Give compiled code an on-stack replacement entry at every loop that ran.                                       |
| `stress-registers`      | Let the register allocator use only the registers of call arguments and results.                               |
| `random-thresholds`     | Give each function a random part of its warm-up and threshold.                                                 |
| `stress-install`        | Install the code of asynchronous compiles after a random number of later tier-up checks.                       |
| `stress-invalidate[=N]` | Invalidate code as if a random dependency stopped holding, at a random one of every N tier-up checks (10 if bare); 0 never does. |

`Tests/LibJS/JIT/modes.py` names the combinations the JIT's test tools run, which compare every mode
with `LIBJS_JIT=off`.

The options are defined in one table, `DEFINITIONS` in `Libraries/LibJS/JIT/Rust/src/options.rs`,
which the parser and `help` both use; a new option goes there and in this section.
