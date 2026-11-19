/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The output of a compile job: machine code and the metadata the runtime
//! needs to install it and to exit from it into the interpreter.

use ak::Column;

use crate::artifact::Artifact;
use crate::artifact::Layout;
use crate::artifact::MAX_WORDS;
use crate::snapshot::CellId;

/// Compiled machine code for one executable, not yet installed. The code and its tables are in memory that comes
/// straight from the system, and that serves the next compile into this, so they are no blocks of the heap.
#[derive(Debug)]
pub struct CompiledCode {
    /// Position independent code, apart from absolute helper addresses,
    /// followed by the constants it loads.
    pub code: Column<u8>,
    /// Where the instructions end and the constants begin.
    pub data_offset: u32,
    /// Offset of the entry point within `code`.
    pub entry_offset: u32,
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
    /// The tables of the code (see `artifact()`), which have `layout`.
    pub words: Column<u32>,
    pub layout: Layout,
    /// The debug dumps `CompileOptions` asked for, as text to print.
    pub dump: Option<String>,
    /// With `CompileOptions::coverage`, what the code contains (see
    /// `coverage::coverage_keys()`).
    pub coverage: Vec<String>,
}

impl Default for CompiledCode {
    fn default() -> Self {
        Self::new()
    }
}

impl CompiledCode {
    /// The most bytes the code of one compile can have. A compile that needs more fails.
    const MAX_CODE_BYTES: usize = 1 << 27;

    /// What a compile writes its code into (see `compile()`). It takes no memory before then.
    pub fn new() -> Self {
        Self {
            code: Column::new(Self::MAX_CODE_BYTES, c"JIT compiled code"),
            data_offset: 0,
            entry_offset: 0,
            site_index_frame_offset: 0,
            register_dump_frame_offset: 0,
            words: Column::new(MAX_WORDS, c"JIT compiled tables"),
            layout: Layout::default(),
            dump: None,
            coverage: Vec::new(),
        }
    }

    /// Drops the code. This keeps its memory for the next compile.
    pub fn clear(&mut self) {
        self.code.clear();
        self.words.clear();
        self.layout = Layout::default();
        self.dump = None;
        self.coverage = Vec::new();
    }

    /// Gives memory back to the system: all that is past the code and its tables, and past the room for
    /// `code_bytes` bytes of code and for `words` words of tables.
    pub fn trim(&mut self, code_bytes: usize, words: usize) {
        self.code.trim(code_bytes);
        self.words.trim(words);
    }

    /// What the runtime reads to install the code, to enter it, and to translate its state into interpreter frames.
    pub fn artifact(&self) -> Artifact<'_> {
        Artifact::new(&self.words, &self.layout)
    }
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
    /// Another thread interrupted the VM, so script has to stop, which the
    /// interpreter does at once. Or the stack got so deep that calls leave
    /// compiled code, which is no reason to stop.
    Interrupted,
    /// Code that a call in an inlined function ran read the legacy
    /// `arguments` of the function's frame, among which were objects the
    /// code never allocated: the runtime created them, and the code exited
    /// after the call (see `Op::AssumeLegacyArgumentsUnread`). Recorded at
    /// the call, in the executable of the function. Recompiles allocate the
    /// objects among the function's arguments at that call.
    LegacyArgumentsRead,
}

impl ExitKind {
    /// Every kind, each at the index that is its discriminant.
    pub const ALL: [Self; 17] = [
        Self::NoFeedback,
        Self::NotObject,
        Self::BadShape,
        Self::BadCallTarget,
        Self::UnexpectedValue,
        Self::SlowPath,
        Self::ArgumentsIndex,
        Self::NotInt32,
        Self::Overflow,
        Self::BadElements,
        Self::OutOfBounds,
        Self::BadType,
        Self::Uninitialized,
        Self::Hoisted,
        Self::Invalidated,
        Self::Interrupted,
        Self::LegacyArgumentsRead,
    ];
}

const _: () = {
    let mut index = 0;
    while index < ExitKind::ALL.len() {
        assert!(ExitKind::ALL[index] as usize == index);
        index += 1;
    }
};

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

/// What kind of object the runtime creates for one that compiled code never allocated (see
/// `artifact::ObjectView`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VirtualObjectDescriptorKind {
    /// A plain object of `shape`, with the values of its properties in
    /// offset order.
    Object { shape: CellId },
    /// A closure like `sample`, the sample of a closure template, with the
    /// lexical and private environments at the two locations, which are
    /// `Repr::Pointer`, the latter null for none.
    Closure { sample: CellId },
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

/// The slot among the values of a frame (see `artifact::FrameView::values()`) that holds the function object of a frame
/// of an inlined closure, whose frames run the closure that was called
/// rather than the function object of the snapshot. It is in no frame slot.
pub const CALLEE_SLOT: u32 = u32::MAX - 1;

/// Where an exit finds a value.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ValueLocation {
    /// A machine register, as saved in the exit's register dump.
    Register(u8, Repr),
    /// A stack slot at the given offset from the JIT frame.
    Stack(i32, Repr),
    /// A slot of the compiled function's frame (an operand) that holds the value wherever compiled code runs, which
    /// is tagged: one of its arguments or its `this` value register that nothing writes.
    Frame(u32),
    /// A NaN-boxed constant, or for the environments of a closure the code never created (see
    /// `VirtualObjectDescriptorKind::Closure`), the address of a cell the code embeds, or null.
    Constant(u64),
    /// The frame's arguments object, which the compiled code never created:
    /// the runtime creates it from the frame's arguments like
    /// `CreateArguments` does, mapped or unmapped, once per exit.
    ArgumentsObject { mapped: bool },
    /// The object at this index among those of the site (see `artifact::SiteView::object()`).
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
    /// The address of a GC cell. In frame states, only as an environment of a
    /// closure the code never allocated (see `VirtualObjectDescriptorKind`).
    Pointer,
}

impl Repr {
    /// Every representation, each at the index that is its discriminant.
    pub const ALL: [Self; 5] = [Self::Tagged, Self::Int32, Self::Float64, Self::Bool, Self::Pointer];
}

const _: () = {
    let mut index = 0;
    while index < Repr::ALL.len() {
        assert!(Repr::ALL[index] as usize == index);
        index += 1;
    }
};
