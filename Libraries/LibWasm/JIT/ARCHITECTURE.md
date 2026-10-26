# The LibWasm optimizing JIT

This document describes how the WebAssembly JIT works. It is a living document: every change to the
compiler's structure updates it. Where it and the code disagree, one of them has a bug.

The compiler is the `libwasm_jit` crate (`Libraries/LibWasm/JIT`). It is a pure compiler: it takes
one function of a validated module and returns machine code, touching no heap and no store, so it
runs on any thread. It depends on the module format (`libwasm_module`, whose sources are the
engine's), not on the engine, which depends on it. Besides the compiler, the crate has what compiled
code calls at run time: the functions for bulk operations (`runtime.rs`), and the stubs the runtime
enters and leaves compiled code through (`stubs.rs`). The engine's side is `runtime/jit.rs` (section
10).

## 1. Overview

A compile (`compile()`) runs these phases on one thread, with a `CompileContext` whose storage is
reused from one compile to the next:

| Phase | Where | What it does |
|---|---|---|
| Decode and build | `decode.rs`, `builder/` | One forward pass over the validated, quickened body. Produces typed SSA with phis, folding and value numbering as it goes. |
| Cleanup | `passes/cleanup.rs` | Removes trivial phis to a fixed point, dead pure nodes, and fuses branches with the comparisons only they use. |
| Loop invariant code motion | `passes/licm.rs` | Finds the nodes of call-free loops that compute the same value in every iteration. |
| Scheduling | `passes/schedule.rs` | Rebuilds the node arena in linear order without the dead nodes, with the invariant nodes at the end of their loop's preheader. |
| Liveness | `regalloc/liveness.rs` | One interval per value in linear order, extended over loops. |
| Allocation | `regalloc/` | One forward walk in linear order. Its verifier simulates every register. |
| Code generation | `codegen/x86_64/`, `codegen/aarch64/` | Lowers every node with its registers, then edge stubs, trap stubs, jump tables and constants. |

`Options::verify_ir` checks the IR after building and after cleanup (`ir/verify.rs`), and the
register allocation (`regalloc/verify.rs`).

The JIT compiles every function that validates, of any size: a hot function that did not compile
would run interpreted forever. Nothing fails for its size, depth, number of locals or register
pressure. The only compile that can fail is an OSR compile at a loop in a legacy catch handler
(`CompileFailure::NoOsrEntry`), whose frame then stays interpreted.

## 2. Decoding

`Decoder` reads one instruction at a time from a validated, quickened body
(`libwasm_engine::quickening`): plain loads and stores access memory 0, which is a memory32 with
guard pages, and the bounds-checked forms are `0xFF` instructions. It reads every instruction the
engine validates, so that the builder can skip unreachable code of any kind. Accesses of other memories
and of memory64s are the bounds-checked `0xFF` forms (`CheckedAccess`); atomic accesses are never
quickened.

## 3. The IR

A `Func` is a list of blocks in linear order, which is the order of the WebAssembly code. Every
forward edge goes to a later block, and every loop is a contiguous range of blocks (`Func::loops`)
that only its header is entered at. That makes linear order a topological order of the forward
edges, so dominators take one pass (`passes/dominators.rs`).

Nodes live in one arena and are named by their index, a `Value`. A `Node` is 16 bytes: an op, a
type, eight bits of flags and three 32-bit arguments, which are inputs or immediates as the op's
`Shape` says. Ops with more inputs keep them in `Func::operands`. A block owns a contiguous range of
nodes, its body, that ends with its terminator; nodes are created in order, so the index of a body
node is its position in linear order. Phis are linked into their block's list through their third
argument, and may sit anywhere in the arena. Successors and predecessors are ranges of flat arrays.

Types: i32, i64, f32, f64, v128, references (zero for null; a funcref is the function's
`FunctionHeader`) and pointers (for addresses the code computes). Constants,
parameters, the instance and the memory base have no code where they are: constants are
rematerialized at every use, and the others arrive in registers or slots at the entry. A node has
no room for a v128 constant, so `Const128` names one in `Func::constants`, as `Shuffle` does its lane
indices.

Effects (`Node::effect()`): `Pure` nodes compute a value and may be shared or removed; `Read` nodes
read memory or globals and may trap; `Write` nodes write state or may trap (division and trapping
truncations, unless their flags say they cannot), and are neither removed nor moved.

| Op | What it does |
|---|---|
| Const32, Const64 | A constant of the node's type (its bits in the arguments). |
| Const128 | A v128 constant, from `Func::constants`. |
| Param, Instance, MemoryBase | The function's parameters, its `InstanceData` and the base of its memory 0. |
| Phi | One input per predecessor, in predecessor order. |
| Forward | A phi that turned out trivial; gone after cleanup. |
| Add ... Rotr, DivS ... RemU, Clz, Ctz, Popcnt, Eqz, Extend*, Wrap | Integer arithmetic of the node's type. Division traps where the divisor is zero, and signed division where it overflows. |
| Compare | An integer comparison (flags: `IntCondition`), as an i32. |
| FAdd ... FNearest, FloatCompare | Float arithmetic and comparison (`FloatCondition`). |
| TruncS, TruncU, TruncSatS, TruncSatU, ConvertS, ConvertU, Demote, Promote, Reinterpret | Conversions to the node's type from the input's. The plain truncations trap on NaN and out of range values. |
| Select | Input 1 where input 0 is nonzero, else input 2. |
| InstanceField, LoadPointer | Pointers in instance data that never change, like `globals` and `globals[i]`, and type identities. |
| Offset | A pointer plus a constant, like the address of a segment. |
| ReadField, WriteField | Fields of runtime data that change, like the size of a memory or table, or a segment's length. |
| TableElement, TableStore | A table's element, after checking the index against its size. |
| GlobalGet, GlobalSet | A global's value, through its storage pointer. Gets of immutable globals are pure. |
| Load, Store | Memory 0 at base + zero-extended index + offset; may trap. With a pointer or a GC object as the base and a zero index, any other address, like a checked access's or a field's. |
| CheckedAddress | The address of an access of a memory without guard pages, or an atomic access, after checking it against the memory's size (and for atomics, its alignment). |
| AtomicRmw, AtomicCompareExchange, AtomicFence | Atomic read-modify-write operations at an address (add, sub, and, or, xor, exchange), compare-exchange, and `atomic.fence`. |
| Rtt | The runtime type of one of the module's types. |
| I31New, I31Get | `ref.i31`, and the value of an i31 reference. |
| ElementAddress | The address of an array's element, after checking the array for null and the index against its length. |
| RefTest | Whether a reference matches a heap type, or for casts, a check that traps where it does not. |
| VectorUnary, VectorBinary, VectorTernary | SIMD operations, by their sub-opcode: splats, lane-wise arithmetic, comparisons, conversions, shifts by an i32, bitselect and the relaxed multiply-adds, and reductions to an i32. |
| ExtractLane, ReplaceLane, Shuffle | Lane access, and `i8x16.shuffle`. |
| VectorLoad | A SIMD load of memory 0, by its sub-opcode (whole, extending, splatting or zero-filling); may trap. |
| CalleeInstance | The instance a call of a function passes: the header's, or the caller's for host functions. Pure only for imports, whose header is never null: a function reference may be null where the call's check does not dominate. |
| CheckFunction, CheckNull | The checks of `call_indirect` (null, then type identity, then the subtype check of GC types) and of references that must not be null. |
| Call | A call through a `FunctionHeader`, direct or not; its value is the first result. |
| Helper | A call of a function of the runtime, which takes its operands in the context and may return a trap code. |
| CallResult | A further result of the call right before it. Never removed, since the results' places depend on them all. |
| Jump, Branch, BranchCompare, BranchFloatCompare, BrTable, Return, Trap, TailCall | Terminators. A branch goes to its first successor where its condition holds. |

## 4. Building SSA

The builder (`builder/mod.rs`) keeps an operand stack of values. Locals become SSA values with
Braun et al.'s algorithm ("Simple and Efficient Construction of SSA Form"), specialized for
structured control flow:

- The merge block of a `block` or `if` is created at its `end`, when every branch to it is known,
  so it is sealed from the start. Branches to a label whose block does not exist yet leave their
  successor slot open, and the `end` fills it in. Only loop headers are unsealed while their body is
  built; their `end` seals them.
- A read of a local first looks at a dense cache of each local's last definition, tagged with its
  block. Older definitions move into a function-wide table from (block, local) to value when the
  cache entry is overwritten. A read that finds neither walks the predecessors with an explicit
  stack, memoizing what it finds, and makes incomplete phis in unsealed loop headers.
- A merge a read cannot come back around to (one outside every loop whose header is sealed) gets a
  phi only if its predecessors' values differ; elsewhere the phi is made first, so that cycles end at
  it, and forwarded if it turns out trivial. A read that reaches a finished loop that never writes
  the local goes straight to the loop's entry: each loop records the locals it and the loops in it
  write. Large functions with many merges and loops otherwise spent most of their build time making
  and forwarding phis nobody needs.
- Phis that turn out trivial become `Forward` nodes, which cleanup removes to a fixed point.
- Values on the operand stack that flow into a merge become phis, as the block type says.
- Unreachable code is skipped by decoding, tracking only nesting.
- After a loop, the code goes on in a block of its own, so that the loop's blocks end where it does.
- The quickened v128 forms of `local.get` and the like (`0xF4` to `0xFA`) build like the plain ones,
  since values carry their types. SIMD instructions keep their sub-opcode in the IR
  (`builder/simd.rs`), so that one node kind per shape carries all of them; the shape comes from the
  module's `simd_kind()` table. Lane loads and stores become scalar loads and stores with lane
  insertions and extractions. Accesses of other memories and of memory64s check their address as the
  core ones do (`CheckedAddress`) and access it with a zero index. Relaxed instructions get the interpreter's semantics: all but the
  multiply-adds and dot products become their deterministic counterparts. A shuffle that uses one
  operand names it twice, with lanes 0 to 15, so the other one need not stay live.

Nodes fold as they are created (`builder/fold.rs`): integer constant folding and algebraic
identities, constants to the right, `eqz` of a comparison as the inverted comparison, and divisions
by constants that cannot trap. Floats are not folded, since NaN payloads are observable.

Pure nodes are value numbered in a table whose entries are undone when the construct that made them
ends, so every hit dominates its use. Constants are numbered function-wide. Global values are
numbered too, keyed by epochs that `global.set` of the global and calls advance; a loop advances
them at its header, since its body may write any global before the header runs again, and
`global.set` makes the stored value the global's known value.

The builder's maps are `table::Table`s, whose emptying costs nothing, so that small functions after
a large one do not pay for its capacity.

## 5. Cleanup

`passes/cleanup.rs` forwards trivial phis to a fixed point, rewrites every use of a forwarded phi,
counts uses, removes pure nodes without uses (transitively), and unlinks dead phis. Then a branch on
a comparison (or `eqz`) in its own block that nothing else uses compares itself
(`BranchCompare`, `BranchFloatCompare`).

## 6. Loop invariant code motion and scheduling

`passes/licm.rs` finds, for each node in a loop, the outermost loop it is invariant in: a pure node
none of whose inputs that loop defines (or each of which moves out of it too), or the value of a
global the loop does not set. Pure nodes never trap, so they may run where the loop would not have.
Memory loads stay, since one could trap. Loops that call keep everything: a call clobbers every
register, so a value computed before the loop would be reloaded from its slot in every iteration,
which costs what computing it again does.

`passes/schedule.rs` then rebuilds the node arena: the entry values first, in the entry block's
range, then every block's live body nodes in order, with the nodes that move to it before its
terminator (the block entering a loop always ends in a jump to its header), and the phis last,
outside every block's range. Inputs are renumbered, so a node's index is its position again, and
dead nodes are gone for the phases after it.

## 7. Register allocation

The allocator (`regalloc/`) follows libjs_jit's: one forward walk over the blocks in linear order.

**Liveness** (`regalloc/liveness.rs`) is one interval per value: from its definition to its last
use, extended over every loop that uses it but does not contain its definition, since loops are
contiguous and entered at their header only. Positions: body node `n` is at `4n + 2`, phis are
defined at `4 * start` of their block, and the moves into a successor's phis happen at `4 * end - 1`,
after the terminator, so that a phi's input is still where the terminator left it. Intervals
over-approximate liveness in the "holes" of structured code, like an `else` arm between a
definition and its use. An interval can even span a merge that some paths reach without the
definition (a `block` the definition's branch skips); the value is dead there, so merges keep only
the values whose definitions dominate them.

**Register classes.** General purpose registers hold integers and pointers; xmm registers hold
floats and vectors alike. A v128 takes 16 bytes in memory: two slots, and 16-byte moves
(`Move::vector`). `Options::stress_registers` gives values only a few registers of each class, so
that most of them spill, for testing.

**The walk.** Each node's constraints (`constraints()`) say which inputs need registers, which may
be immediates, which need fixed registers, what it clobbers and where its output goes. Fixed inputs
and temps are placed first; an input of the same node in the way moves to another free register.
When no register is free, the value used furthest in the future is evicted. Inside a loop, the
values the rest of the iteration uses come first, then those the next iteration reads, then those
used after the loop. Values first used by a call argument or a return value prefer that register.

**Spilling** happens at the definition: a value that ever needs a stack slot gets one for its whole
life, and code generation stores it right after defining it, so a slot always holds its value once
it has one. Parameters use their home slots and the instance its frame header slot. Every value
live across a call gets a slot, since calls clobber every register. A phi and its inputs (its web)
share slots wherever their intervals do not overlap, so that the moves into a spilled phi on its
edges, a loop's back edge above all, are mostly no moves at all.

**Blocks.** A block with one predecessor starts with that predecessor's registers. A merge starts
with those of the predecessor that keeps the most of its values in registers, so that a path that
called a function, which leaves none in registers, does not make the others give theirs up; a loop
header starts with those of the block entering the loop, without the values the loop only carries
through that have a slot already. Their phis get registers free there, preferably the input's own
from that predecessor. Every other edge moves its values into place, as parallel moves with cycles
broken through a slot; back edges get theirs when their source block ends.

**The verifier** (`regalloc/verify.rs`) simulates every register from the state each block starts
with, checks that every input is where the allocation says, and that every edge's moves produce its
target's state, with phis replaced by their inputs. It tracks what each block writes to slots, takes
a slot the block did not write to hold the one of its values live there, and checks that values
sharing a slot are never live at once.

## 8. Calls and frames

`convention.rs` defines the calling convention between compiled functions on x86-64:

- Integer parameters go in rdi, rsi, rdx, rcx, r8 and r9, float and vector ones in xmm0 to xmm7, in
  order. Every parameter has a home slot in the caller's outgoing area, at the callee's fp + 8 * the
  slots of the parameters before it (a v128 takes two, as on the interpreter's operand stack), where
  the parameters that do not fit in registers are passed.
- Integer results come back in rax and rdx, float and vector ones in xmm0 and xmm1. The others go to
  a result area whose address the caller passes in the slot after the parameters' home slots, each
  at 8 * the slots of the results before it.
- r14 holds the callee's instance, r15 the base of its memory 0, and r10 its `FunctionHeader`,
  whose native entry the call goes through.
- Every register but rsp and rbp is caller-saved, so the instance and memory base are SSA values
  the allocator may spill like any other. rsp is 16-byte aligned at the call.
- i32 values are zero-extended to 64 bits in registers and slots, so memory indices serve as they
  are; every i32 operation is a 32-bit instruction, which zero-extends its result.

A compiled frame has fp = the caller's rsp at the call: the return address at fp - 8, the caller's
fp at fp - 16 (the order of the interpreter's frame headers too), the instance
at `FRAME_INSTANCE`, then the spill slots, and the outgoing area at rsp. After a call, rsp is
recomputed from fp, which also undoes whatever a callee's tail calls did to it.

The prologue writes the instance to the frame and checks the new rsp against the stack limit of
the store's `ExecutionContext`, which `InstanceData::context` points at, trapping with
`TRAP_STACK_EXHAUSTED`. `RuntimeInfo` has the trap exit.

**Calls.** Every call goes through `FunctionHeader::native_entry`. A direct call of a function of
the same instance passes the caller's instance and memory base; any other call (imports,
`call_indirect`, `call_ref`) passes the callee's, from its header (`CalleeInstance`). `call_indirect`
checks the index against the table's size, the element for null, and its type identity against the
expected one inline; a mismatch goes to an out-of-line check of the runtime types' displays, which
only GC modules' subtypes pass.

**Host functions** have a host stub of their signature as their native entry (`stubs.rs`). It
builds a frame like a compiled function's, lays the arguments out as the interpreter's operand stack
would hold them, calls `wasm_runtime_call_host`, and loads the results. A host function has no
instance, so it is called with the caller's, through which the stub finds the context. The frame it
passes as the one an exception unwinds from has no instance, so that the runtime does not walk
compiled frames.

**Tail calls** place their operands as a call does, then copy the stack arguments (and the result
area pointer, which the caller's caller passed) from the outgoing area into the incoming one, from
the highest slot down, since the incoming area is the higher one. If they need more slots than the
function was called with, the return address and saved fp move down first by the difference,
rounded to 16 bytes. Then they restore fp and jump to the callee's native entry.

**Runtime functions** (`Helper`) take the instance in rdi and their operands in the next argument
registers, which the code stores into the context's `arguments`; they clobber what the host ABI
does not preserve, so values stay in rbx and r12 to r15 across them. Growing calls the engine's
helpers; fill, copy and init of memories and tables call the JIT's (`runtime.rs`), which return 0 or
the trap code.

**Memories without guard pages and atomics** (`builder/memories.rs`). An access of a memory64, or of
a memory other than memory 0, is a `CheckedAddress` and a `Load` or `Store` at it. `CheckedAddress`
computes the end of the access, index + offset + size, in 64 bits, trapping where that wraps around
(only a memory64's can), compares it with the memory's size, and returns base + end - size. Atomic
accesses take the same path for every memory, and check the end for alignment too, after the bounds,
as the interpreter does. Atomic loads are plain loads, and stores are exchanges, as in the
interpreter's lowering: on x86-64 that makes both sequentially consistent. Add, sub and exchange are
`lock xadd` and `xchg`; and, or and xor are `lock cmpxchg` loops. `atomic.fence` is `mfence`. Waiting
and notifying call the runtime; whether the memory is shared is part of its type, so waiting on an
unshared memory traps, and notifying one wakes nobody, without a call.

**GC** (`builder/gc.rs`). Objects are cells of the runtime's heap: the Rtt first, the address of the
data, an array's length, then the data inline if it has at most 4096 bytes. A struct's fields are at
constant offsets (8 bytes each, 16 for v128, packed and 32-bit values zero-extended in their word), so
`struct.get` is a null check and a load at the object plus the field's offset; a large struct's data,
or the data of any struct accessed through a type that is not final, is reached through its address
first. `array.get` and `array.set` access the element at its
`ElementAddress`. Allocation, filling, copying and initializing from segments call the runtime
(`Helper`); the allocation traps with `TRAP_RUNTIME` where the runtime returns null. `ref.test`,
`ref.cast`, `br_on_cast` and `br_on_cast_fail` are a `RefTest`: abstract heap types test the
reference's bits (an i31 has bit 0 set, a host value bits 1:0 equal to 0b10, a GC object neither) and
the kind of the object's Rtt; concrete types compare the type's identity with the actual type's
display at the expected type's depth, which the module's subtype chain gives at compile time.

GC references in compiled frames need nothing of their own: the collector scans the stack
conservatively, recognizes pointers into cells (so tagged references keep their cells alive), and
every allocation is a call of the runtime, around which values live in stack slots or callee-saved
registers. The one hazard is a pointer to data outside the heap, which does not keep its object
alive; `ReadField` and `ElementAddress` are not pure, so such pointers are computed where they are
used, while the object is live.

## 9. Code generation

There is a backend per target, with the same structure: `codegen/x86_64/` and `codegen/aarch64/`
(ARMv8.1 with LSE atomics and NEON), each with its own instruction selection for numbers, vectors,
memory, calls, tables and GC objects. The rest of this section describes the x86-64 one.

`codegen/x86_64/` lowers the blocks in linear order for an x86-64-v2 baseline (SSE4.2 and POPCNT).
The assembler and allocator select optional instructions independently using runtime CPU detection:
AVX for scalar floats, AVX2 for vectors, BMI2 for shifts and rotates, and LZCNT/BMI1 for zero counts.
Without them, `asm/x86_64/sse.rs` emits legacy SSE sequences and bit scans, and variable integer
shifts use CL. This selection also covers entry, exit and host stubs. The shared encoders live in
libjs_jit's `asm/x86_64/`; `codegen/x86_64/vector.rs` names the SIMD opcodes. SSE packed operations
read spilled vectors through registers, since their memory operands require sixteen-byte alignment.
Distributed C++ and Rust builds target the same v2 baseline (`ENABLE_CI_BASELINE_CPU`); native
builds still target the machine building them. After each node, a value with a slot is stored
there; a phi in a register with a slot is stored where its block starts.

- **Vectors.** A vector node's output register differs from its inputs and temps, so lowerings may
  write it first; xmm15 is their scratch, and `vector_temps()` says which take an extra register.
  Where the interpreter's instruction sequence decides bits the spec leaves open (NaN payloads of
  min and max, relaxed instructions), the lowering is the same sequence, so both tiers agree bit for
  bit. Shift counts that are constants use the immediate forms, v128 constants that are an
  instruction's r/m operand stay in the constant pool (`vector_constant_inputs()`), and shuffles
  become one `vpshufd`, `vpalignr`, `vpunpck*dq`, `vshufps`, `vpblendd` or `vpshufb` where they can,
  else two `vpshufb` and an or.

- **Edges.** A jump moves its values and jumps unless its target comes next. A two-way branch whose
  taken edge needs moves jumps to a stub that moves them; its other edge moves inline. Where the
  taken target comes next and needs no moves, the condition is inverted. `br_table` is a jump table
  of 32-bit offsets, with stubs for the edges that need moves.
- **Traps** jump to one stub per trap code, which puts the code in edi and jumps to
  `RuntimeInfo::trap_exit` with rbp still the trapping frame's fp. Memory accesses rely on guard
  pages: the runtime's fault handler turns faults in compiled code into the same trap exit.
- **Data**: float constants and masks live in a constant pool after the code, jump tables before
  it, both reached rip-relative, so the code is position independent. The runtime is reached
  through absolute addresses.

## 10. The runtime

The engine's `runtime/jit.rs` decides what is compiled, installs it, and connects compiled code with
the interpreter and the host. The JIT is on by default; `LIBWASM_JIT` configures it (`off`, `forced`,
`eager`, `threshold=N`, `perf-map`, the stress modes; see the module's documentation).

- **Tier-up.** The interpreter charges each function's `tier_up_budget` for calls and loop
  iterations and asks the runtime once it runs out. The runtime sends the compile to the process's
  compile threads and lets the interpreter go on, asking again after a small budget; it installs the
  code once it is done. At a loop, it continues the frame compiled (OSR): an OSR compile enters the
  outermost loop around the frame's position and branches down to the loop the frame is at.
- **Exceptions.** Compiled code handles its own handlers in IR. An exception that leaves compiled
  code is unwound over the compiled frames to the first call that checks for exceptions or to the
  entry trampoline, and from there goes on in the interpreter frame that called it.

- **Installing** a function's code stores it in its header: `native_entry`, and the kind
  `FUNCTION_KIND_COMPILED`. Code is per module function, so each store compiles a module's function
  once for all its instances. Each store has its own code space of executable chunks.
- **Interpreter to compiled.** The interpreter's `call_function` hands a compiled callee to the
  runtime helper `wasm_runtime_call_compiled`, which reads the arguments off the operand stack and
  calls the code through the entry trampoline (a `NativeCall`: registers, and an image of the home
  slots and result area), then puts the results where the arguments were.
- **Compiled to the rest.** Every other function's native entry is the runtime adapter, which saves
  the argument registers and calls `wasm_runtime_call_from_native`. That runs the callee through
  `Store::invoke` (a nested interpreter entry, or the host function) and puts the results into the
  saved registers and result area, or returns a trap code, keeping the trap in the store. A tail
  call of WebAssembly code from compiled code that the entry trampoline called, whose adapter frame
  returns to the trampoline, is not made there: the runtime keeps the callee and its arguments and
  returns through the trampoline, and the interpreter calls the callee in the compiled function's
  place (its CallPending pseudo-instruction, or a tail call of the frame an OSR compile continued).
  So functions that tail call each other across the tiers run in constant stack space.
- **Traps.** The trap exit stores the code in the context's first argument and the context in the
  trapping frame's header, then continues at the interpreter's TrapFromNative pseudo-instruction
  (opcode 0xEF), which traps like the interpreter does: out of the innermost interpreter entry,
  past the compiled frames and the helpers' frames. Memory faults in compiled code go to the trap
  exit too: the fault handler knows the code spaces from a lock-free range set (`CODE_RANGES`).

## 11. Testing

- `cargo test -p libwasm_jit`: unit tests, and the decoder and execution tests over the spec
  testsuite's modules when the build directory has them.
- `examples/spec_check.rs` (and `tests/execution.rs`, with and without `stress_registers`) runs the
  spec testsuite's scripts in two stores, one where calls into compiled functions run the compiled
  code, and compares results, traps, memories, tables (by which elements are null) and globals with
  the interpreter's. `tests/harness` has a session per thread: a code space whose start is the trap
  exit, a trampoline per signature, host stubs, and a stub for functions that did not compile, which
  traps with a code of its own; binding a module's instance points the engine's function headers at
  the code. With `--runtime`, the script calls go through `Store::invoke` instead, and the runtime
  compiles what `LIBWASM_JIT` says (`eager` by default).
- To check the SSE path on an AVX-capable development machine, build `spec_check` with
  `RUSTFLAGS='-Ctarget-cpu=x86-64-v2' cargo build -p libwasm_jit --example spec_check`, then run
  `qemu-x86_64 -cpu Nehalem target/debug/examples/spec_check SCRIPT_DIRECTORY`. Repeat with
  `--stress-registers` to exercise spills and with `--runtime` to exercise the runtime's tier
  transitions and stubs. `-cpu SandyBridge` also checks AVX scalar floats alongside SSE vectors.
- `examples/bench_check.rs` runs a module's entry in an interpreted store and a compiled one, with
  deterministic imports, and compares results, exit codes, memories, tables and globals.
- `examples/smith_check.rs` does the same for random modules from `wasm-tools smith` in the subset
  the JIT compiles, calling every exported function with random arguments.
- `examples/compile_all.rs` compiles every function of modules and reports coverage, the CPU time
  of compiling the functions the JIT compiles on one thread and on several, per byte and per phase
  (`Options::stats`), the slowest large function, the cost of small ones, and nodes, slots and moves
  per byte; with `--verify` it checks every phase.
