/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The output of a compile job: machine code and the metadata the runtime
//! needs to install it and to exit from it into the interpreter.

use crate::snapshot::CellId;

/// Compiled machine code for one executable, not yet installed.
#[derive(Debug, Clone, Default)]
pub struct CompiledCode {
    /// Position independent code, apart from absolute helper addresses,
    /// followed by the constants it loads.
    pub code: Vec<u8>,
    /// Where the instructions end and the constants begin.
    pub data_offset: u32,
    /// Offset of the entry point within `code`.
    pub entry_offset: u32,
    /// On-stack replacement entry points, as (pc, offset within `code`): a
    /// frame running in the interpreter that is at the loop back edge `pc`
    /// can continue there, with the same calling convention as the entry
    /// point.
    pub osr_entries: Vec<(u32, u32)>,
    /// The loop back edges from where a frame running in the interpreter
    /// reaches one of the `osr_entries` without leaving the loop it is in.
    pub osr_loop_back_edges: Vec<u32>,
    /// The loop back edges in the loops around those of the `osr_entries`,
    /// from where a frame running in the interpreter may reach one of them
    /// on a later iteration.
    pub osr_outer_loop_back_edges: Vec<u32>,
    /// See `ir::Graph::uncaught_handlers`.
    pub uncaught_handlers: Vec<u32>,
    /// The sites of the code, by index: exit and leave stubs pass the index of
    /// theirs to the runtime, and calls in inlined callees name theirs.
    pub sites: Vec<Site>,
    /// Cells the code depends on (compares against or uses). The runtime keeps
    /// them alive for as long as the code may run.
    pub embedded_cells: Vec<CellId>,
    /// What the code assumes keeps holding. The runtime installs the code
    /// only if all of them still hold, and invalidates it when one stops
    /// holding.
    pub dependencies: Vec<Dependency>,
    /// What invalidates the code: the bytes to write at each offset of it
    /// (see `Op::AssumeValid`).
    pub invalidation_patches: Vec<(u32, Vec<u8>)>,
    /// The offset from the frame pointer of the 32-bit word where the code
    /// keeps the index of a site the runtime reads the values of: that of
    /// an exit or leave, while a slow path in an inlined callee runs in
    /// frames the runtime published, that of their publication, and while a
    /// call in an inlined callee runs in frames the runtime materialized for
    /// it, that of the call.
    pub site_index_frame_offset: i32,
    /// The offset from the frame pointer of the register dump (see
    /// `ValueLocation::Register`) that exits, leaves and publications fill,
    /// which holds the values of a publication while its slow path runs.
    pub register_dump_frame_offset: i32,
    /// The debug dumps `CompileOptions` asked for, as text to print.
    pub dump: Option<String>,
    /// With `CompileOptions::coverage`, what the code contains (see
    /// `coverage::coverage_keys()`).
    pub coverage: Vec<String>,
}

/// The status word of `JitResult { u64 value; u64 status; }`, returned by
/// compiled code in the second return register.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u64)]
pub enum JitStatus {
    /// `value` is the return value. The frame is still on the interpreter
    /// stack; the caller pops it like the interpreter's `Return` would.
    Returned = 0,
    /// Continue interpreting the running execution context at its
    /// `program_counter`.
    Resume = 1,
    /// An exception propagates out of the executable, as when a slow path
    /// returns a negative control word.
    ExitInterpreter = 2,
}

/// Why compiled code exits to the interpreter. Exit sites are recorded per
/// executable as `(pc, ExitKind)` so the same speculation is never repeated.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ExitKind {
    /// The bytecode never ran in the interpreter, so it was compiled as an
    /// unconditional exit.
    NoFeedback,
    /// A value speculated to be an object was not one.
    NotObject,
    /// An object did not have one of the shapes its property cache had seen,
    /// or a cached property turned out to be an accessor, or a cached
    /// prototype chain was invalidated.
    BadShape,
    /// A call inlined for the callee its call feedback had seen called
    /// another function.
    BadCallTarget,
    /// A value the compiled code had folded to the constant it held at
    /// compile time (a method on a prototype, the key of a keyed access)
    /// was another value.
    UnexpectedValue,
    /// An instruction whose slow path cannot run from compiled code, or that
    /// was compiled to exit instead of running its slow path, met a case only
    /// its slow path handles (like an append that needs to grow an array, a
    /// probe that cannot tell, or `Math.abs` of a string), so the
    /// interpreter runs it.
    SlowPath,
    /// An `arguments[i]` the compiled code read from the frame's arguments
    /// had an index that is not an int32 within the passed arguments.
    ArgumentsIndex,
    /// A value speculated to be an int32, from the arithmetic feedback of
    /// the instruction using it, was not one.
    NotInt32,
    /// Int32 arithmetic speculated not to overflow, from the instruction's
    /// arithmetic feedback, overflowed or produced -0.
    Overflow,
    /// An object accessed by index did not have the elements kind (packed or
    /// holey array, or typed array of one kind) its keyed feedback had seen.
    BadElements,
    /// An index was out of the bounds of the elements, or hit a hole, or a
    /// keyed store compiled to exit instead of running its slow path met a
    /// case only its slow path handles.
    OutOfBounds,
    /// A value had another type than the instruction's feedback had seen.
    BadType,
    /// A binding or local variable speculated to be initialized was not, so
    /// the interpreter throws its ReferenceError.
    Uninitialized,
    /// A node that may exit, which loop invariant code motion moved out of a
    /// block that not every iteration of its loop runs, exited before the
    /// loop. Exits of this kind are recorded at the loop header, and keep
    /// such nodes in that loop, but not the nodes from blocks every iteration
    /// runs.
    Hoisted,
    /// Something the code depended on stopped holding while it ran other
    /// code (see `Dependency`), so the code was invalidated.
    Invalidated,
    /// Code that a call in an inlined function ran read the legacy
    /// `arguments` of the function's frame, among which were objects the
    /// code never allocated: the runtime created them, and the code exited
    /// after the call (see `Op::AssumeLegacyArgumentsUnread`). Recorded at
    /// the call, in the executable of the function. Recompiles allocate the
    /// objects among the function's arguments at that call.
    LegacyArgumentsRead,
}

/// Something compiled code assumes keeps holding, without checking it where
/// it relies on it. The runtime invalidates the code when it stops holding:
/// every `Op::AssumeValid` of the code then exits, and the code is
/// discarded. Each kind stops holding in the runtime event that every fast
/// path breaking it needs first, so fast paths never break it unnoticed.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Dependency {
    /// The `WatchpointSet` cell stays valid.
    WatchpointSetValid(CellId),
    /// The global declarative environment `environment` keeps the serial
    /// number `serial`: it gets no bindings, which could shadow properties of
    /// the global object.
    GlobalDeclarations { environment: CellId, serial: u64 },
    /// Binding `index` of the global declarative environment `environment`
    /// is never assigned after its initialization.
    GlobalBindingUnassigned { environment: CellId, index: u32 },
    /// The own property at `offset` of the global object `object`, which
    /// holds the value it got first that is not undefined, is never
    /// assigned and never deleted. The global object shares its shape with
    /// no other object while it is a dictionary, so that nothing but the
    /// runtime writes to the property until it is assigned.
    GlobalPropertyUnassigned { object: CellId, offset: u32 },
    /// No object has the `[[IsHTMLDDA]]` internal slot (`document.all`), so
    /// no object is falsy, loosely equal to null, or of type "undefined".
    NoHtmlDdaObjects,
    /// The shape stays stable: no object leaves it, so objects that have it
    /// keep it while other code runs.
    StableShape(CellId),
}

/// A point of compiled code where the runtime may translate its state into
/// interpreter frames (see `SiteKind`): the frame states there, and where
/// their values are.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Site {
    pub kind: SiteKind,
    /// The frames there, innermost first: the frames of the inlined calls
    /// the site is in, and the compiled function's frame.
    pub frames: Vec<FrameState>,
    /// The objects the compiled code never allocated, which the frames'
    /// values refer to (as `ValueLocation::VirtualObject`).
    pub objects: Vec<VirtualObjectDescriptor>,
    /// For a `SiteKind::Call`: the room for the frames of the inlined calls
    /// that the call leaves below its callee's frame (see `Op::CallDirect`).
    pub inlined_frame_bytes: u32,
}

/// What a site of compiled code is. The runtime translates the state of
/// compiled code at any of them into interpreter frames the same way.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SiteKind {
    /// Compiled code exits to the interpreter, for this reason, which the
    /// executable records so that its recompiles do not repeat what failed.
    Exit(ExitKind),
    /// A slow path did not continue in compiled code: the runtime writes the
    /// frame state into the compiled function's frame and the frames
    /// compiled code pushed for a slow path in an inlined callee, as far as
    /// they are still on the stack. No exit: nothing is recorded, and only
    /// an exception that a handler of an inlined callee caught is counted.
    Leave,
    /// A slow path or call in an inlined callee, which runs in the frames of
    /// the inlined calls with its operands as values: the runtime pushes
    /// those frames with their headers only (Header translation: the
    /// function, `this`, the arguments and the pc), uninitialized, and the
    /// slow path runs in the innermost one. Compiled code pops them again
    /// if the slow path continues in it; otherwise a leave writes them.
    Publish,
    /// A call in an inlined callee that runs without the frames of the
    /// inlined calls it is in (see `Op::CallDirect`), with its values in
    /// stack slots of the JIT frame. Stack walks show those frames from it,
    /// and the call's slow paths materialize them.
    Call,
}

/// An object the compiled code never allocated, which an exit creates: a
/// plain object of `shape` with the values of its properties in offset
/// order. Each one is created once per exit, before any is given its
/// properties, so objects may refer to each other.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VirtualObjectDescriptor {
    pub shape: CellId,
    /// The node index of the allocation the object stands for, which is the
    /// same at every site of the code: the runtime may have created the
    /// object at another site of the same frame already, and then uses that.
    pub allocation: u32,
    pub properties: Vec<ValueLocation>,
}

/// Set in the return pc (`ExecutionContext::caller_return_pc`) of the frame of
/// a callee that a call in an inlined callee made without the frames of the
/// inlined calls it is in (see `SiteKind::Call`): the other bits are the
/// index of the call's site in the code of the caller frame, which runs the
/// compiled function. The frame's destination
/// (`ExecutionContext::caller_dst_raw`) is then the low half of the JIT
/// frame's frame pointer, from which the site's stack slots are, while the
/// call runs.
pub const INLINED_CALL_SITE_BIT: u32 = 1 << 31;

/// Where the interpreter resumes a frame after an exit.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ResumeMode {
    /// Eager exit: rerun the bytecode at `pc` from its pre-state.
    ResumeAt,
    /// Lazy exit after a call: continue after the bytecode at `pc`, with the
    /// call's result stored in the slot `dst`.
    ResumeAfter { dst: u32 },
}

/// The slot of `FrameState::values` that holds the function object of a frame
/// of an inlined closure, whose frames run the closure that was called
/// rather than the function object of the snapshot. It is in no frame slot.
pub const CALLEE_SLOT: u32 = u32::MAX - 1;

/// The interpreter state of one frame at an exit.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FrameState {
    /// Index into `Snapshot::executables`.
    pub executable: u32,
    pub pc: u32,
    pub mode: ResumeMode,
    /// The value of each frame slot that differs from what the frame already
    /// holds, in slot order.
    pub values: Vec<(u32, ValueLocation)>,
    /// The live slots whose value the frame already holds (see
    /// `ir::FrameState::in_frame`), in slot order. Exits clear the registers
    /// and locals listed in neither, except a `ResumeAfter` destination,
    /// which exits never write either.
    pub in_frame: Vec<u32>,
    /// See `ir::FrameState::passed_argument_count`.
    pub passed_argument_count: Option<u32>,
    /// The argument slots that the legacy `arguments` of the frame's
    /// function shows (see `ir::FrameState::legacy_arguments`). Objects the
    /// code never allocated among them are only created when code reads them
    /// (see `ir::Op::AssumeLegacyArgumentsUnread`).
    pub legacy_arguments: core::ops::Range<u32>,
}

/// Where an exit finds a value.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ValueLocation {
    /// A machine register, as saved in the exit's register dump.
    Register(u8, Repr),
    /// A stack slot at the given offset from the JIT frame.
    Stack(i32, Repr),
    /// A NaN-boxed constant.
    Constant(u64),
    /// The frame's arguments object, which the compiled code never created:
    /// the runtime creates it from the frame's arguments like
    /// `CreateArguments` does, mapped or unmapped, once per exit.
    ArgumentsObject { mapped: bool },
    /// The object at this index of `Site::objects`.
    VirtualObject(u32),
}

/// How a value is represented in a register or stack slot.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Repr {
    /// A NaN-boxed JS value.
    Tagged,
    Int32,
    Float64,
    Bool,
    /// The address of a GC cell. Never in frame states.
    Pointer,
}
