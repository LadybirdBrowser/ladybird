/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Everything the compiler gets to see, captured on the main thread.
//!
//! A snapshot is plain data: it never points into the GC heap. Cells are
//! identified by `CellId`, which is only ever compared. The main thread keeps
//! every referenced cell alive until the compiled code is installed or the
//! compile job is abandoned.

use crate::bytecode::ExceptionHandler;
use crate::bytecode::FrameLayout;
use crate::code::ExitKind;

/// The address of a GC cell on the main thread, used as an opaque identity.
/// It is never dereferenced by the compiler.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct CellId(pub u64);

/// One bytecode executable: the function being compiled (index 0) or an
/// inlining candidate.
///
/// The runtime's snapshot builder (`Libraries/LibJS/Rust/src/jit/snapshot.rs`)
/// adds inlining candidates for every Call feedback slot whose monomorphic
/// target qualifies (an ECMAScript function the interpreter can call inline,
/// without a function environment or `this` resolution through the
/// environment, not a class constructor, generator or async function, without
/// direct eval, in the caller's realm), recursively up to the inlining depth,
/// each with at most `LIBJS_JIT=inline-max-size=N` bytecode instructions. The
/// graph builder applies the inlining budget. Each function object is added
/// once, so the same index can be the target of several slots.
#[derive(Debug, Clone, Default)]
pub struct ExecutableSnapshot {
    /// A copy of the assembled bytecode, exactly as the interpreter runs it.
    pub bytecode: Vec<u8>,
    /// The address of the executable's own bytecode on the main thread
    /// (`Executable::bytecode.data()`). Slow paths receive pointers to
    /// instructions in it, which the code embeds as immediates.
    pub bytecode_address: u64,
    pub layout: FrameLayout,
    /// NaN-boxed constant values, in constant slot order.
    pub constants: Vec<u64>,
    pub exception_handlers: Vec<ExceptionHandler>,
    /// Places where speculation already failed. The compiler never repeats a
    /// speculation of the same kind at the same pc.
    pub exit_sites: Vec<(u32, ExitKind)>,
    /// For the compiled executable only: places in builtins written in
    /// JavaScript (by their executable's cell) where its code exited while
    /// running them inlined. Code inlining a builtin consults these instead
    /// of the builtin's `exit_sites`, which come from all its callers.
    pub builtin_exit_sites: Vec<(CellId, u32, ExitKind)>,
    pub feedback: FeedbackSnapshot,
    /// The interpreter's property lookup caches, indexed like
    /// `Executable::property_lookup_caches`.
    pub property_caches: Vec<PropertyCacheSnapshot>,
    /// The `Executable` cell itself.
    pub cell: CellId,
    /// The function object of an inlining candidate; `None` for index 0.
    pub function: Option<InlinedFunctionSnapshot>,
    /// The words of `function` its frames start with (see
    /// `FunctionFrameFields`), if known, for code that pushes frames of it.
    /// For index 0, those of the compiled function.
    pub function_fields: Option<FunctionFrameFields>,
    /// The `[[Environment]]` of `function`, which stays the same for the
    /// function's lifetime. Inlining candidates need no function
    /// environment, so it is the lexical and variable environment of their
    /// frames.
    pub environment: Option<CellId>,
    /// Whether `function` is a builtin written in JavaScript. Its frames are
    /// set up like those of other builtins: with the environments of their
    /// caller's frame, and without a script or module.
    pub builtin: bool,
    /// Whether the function's mapped arguments object (made by `CreateArguments`
    /// of the mapped kind) aliases parameters, so that it changes when they do.
    pub mapped_arguments_alias_parameters: bool,
    /// The shape `NewObject` gives an object without a cached shape: an
    /// empty shape with `%Object.prototype%` of the executable's realm.
    pub new_object_shape: Option<ShapeSnapshot>,
    /// `%String.prototype%` of the executable's realm, where `GetById`
    /// looks up the properties of strings, which have no own properties
    /// besides `length` and their indices (that `GetById` never names).
    pub string_prototype: Option<CellId>,
    /// `%Number.prototype%` of the executable's realm, where `GetById`
    /// looks up the properties of numbers, which have no own properties.
    pub number_prototype: Option<CellId>,
    /// `%Boolean.prototype%` of the executable's realm, where `GetById`
    /// looks up the properties of booleans, which have no own properties.
    pub boolean_prototype: Option<CellId>,
    /// The shapes object literals reached before (`NewObject` with a cache
    /// index creates its object with that shape if there is one), indexed
    /// like `Executable::object_shape_caches`.
    pub object_shape_caches: Vec<Option<ObjectShapeCacheSnapshot>>,
    /// For the compiled function only: the closures its `NewFunction`
    /// instructions create, indexed by `shared_function_data_index`, if JIT
    /// code can create them by copying (see `FunctionAllocationInfo`).
    pub closure_templates: Vec<Option<ClosureTemplateSnapshot>>,
    /// For the compiled function only: how JIT code allocates the
    /// environments of its `CreateLexicalEnvironment` instructions itself,
    /// indexed by their environment shape cache, if it can.
    pub lexical_environment_templates: Vec<Option<LexicalEnvironmentTemplateSnapshot>>,
    /// The environments of direct calls (see `DirectCallTarget::environment`).
    pub environment_templates: Vec<EnvironmentTemplateSnapshot>,
    /// For the compiled function only: its identifier table, as the words of
    /// its `Utf16FlyString`s, which the executable keeps alive.
    pub identifiers: Vec<u64>,
    /// The global variables of the executable's realm that its `GetGlobal`
    /// and `SetGlobal` instructions access, if code may access them directly.
    pub globals: Option<GlobalsSnapshot>,
}

/// The global object and the global declarative environment of a realm, and
/// what the global variable caches of an executable running in it know.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct GlobalsSnapshot {
    pub object: CellId,
    /// The declarative environment record of the global environment, which
    /// holds the global `let`, `const` and `class` bindings.
    pub declarative_environment: CellId,
    /// The serial number of `declarative_environment`, which changes when
    /// it gets bindings, such as those a later script declares, which may
    /// then shadow properties of the global object.
    pub environment_serial: u64,
    /// Indexed like `Executable::global_variable_caches`: what each cache
    /// found, if it still applies.
    pub caches: Vec<Option<GlobalCacheSnapshot>>,
}

/// What a global variable cache found.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GlobalCacheSnapshot {
    /// Binding `index` of the global declarative environment, which keeps
    /// its bindings at their indices, with its value unless it was still
    /// uninitialized. Immutable bindings (`const`) keep their value once
    /// initialized, and so do mutable ones while nothing assigns them.
    Binding {
        index: u32,
        mutable: bool,
        /// Whether the binding may have been assigned since its
        /// initialization.
        assigned: bool,
        value: Option<GlobalValueSnapshot>,
    },
    /// The own data property at `offset` of the global object, which had
    /// `shape`, with its value. No binding of the global declarative
    /// environment shadows it while its serial number stays the same.
    Property {
        shape: CellId,
        /// For dictionary shapes, the generation the shape must still have.
        dictionary_generation: Option<u32>,
        offset: u32,
        /// Whether a `SetGlobal` may store into the property, which is a
        /// writable data property.
        writes_data_property: bool,
        /// Whether the property may have been assigned since it got the
        /// first value that is not undefined (see
        /// `Dependency::GlobalPropertyUnassigned`).
        assigned: bool,
        value: GlobalValueSnapshot,
    },
}

/// The value of a global variable when the snapshot was taken.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GlobalValueSnapshot {
    /// The NaN-boxed value.
    pub bits: u64,
    /// The value's cell, for values that are cells, which code that folds
    /// the value embeds.
    pub cell: Option<CellId>,
    /// Which intrinsic the value is, if it is one the compiler knows.
    pub intrinsic: Option<Intrinsic>,
    /// How `instanceof` with the value on its right-hand side runs, if the
    /// value is a function that inherits `%Function.prototype%`'s
    /// `@@hasInstance` (which can never change): OrdinaryHasInstance with its
    /// `prototype` property.
    pub has_instance: Option<OrdinaryHasInstanceSnapshot>,
}

/// A function whose `@@hasInstance` is `%Function.prototype%`'s, for as long
/// as it has `shape` (which has no `@@hasInstance` property, and has
/// `%Function.prototype%` as its prototype), with its `prototype` data
/// property at `prototype_offset`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct OrdinaryHasInstanceSnapshot {
    pub shape: CellId,
    /// For dictionary shapes, the generation the shape must still have.
    pub dictionary_generation: Option<u32>,
    pub prototype_offset: u32,
}

/// A function object like the closures of one `NewFunction` instruction,
/// apart from their environments.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ClosureTemplateSnapshot {
    /// The function object itself. Code that copies it embeds it, which
    /// keeps everything it refers to alive.
    pub sample: CellId,
    /// Its bytes (see `FunctionAllocationInfo`).
    pub words: Vec<u64>,
    /// The index in `Snapshot::executables` of the closures' executable, if
    /// calls of them can be inlined.
    pub inline_executable: Option<u32>,
}

/// An `ObjectShapeCache` that has a shape.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ObjectShapeCacheSnapshot {
    pub shape: ShapeSnapshot,
    /// Where `InitObjectLiteralProperty` with each `property_slot` stores
    /// its value in objects of `shape`, as far as the cache knows. A slot
    /// without an offset here has none known.
    pub property_offsets: Vec<u32>,
}

/// A shape objects can be created with. Code that does so embeds it and must
/// list it in `CompiledCode::embedded_cells`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ShapeSnapshot {
    pub shape: CellId,
    /// How many named properties objects of this shape have. JIT code can
    /// only allocate an object itself if they fit a size class
    /// (`ObjectAllocationInfo::size_class_for()`).
    pub property_count: u32,
}

/// The function object an inlining candidate executable belongs to. Code that
/// inlines it embeds `function` (the call target check compares against it)
/// and must list it in `CompiledCode::embedded_cells`.
///
/// To materialize the callee's frame, JIT code needs room on the interpreter
/// stack for `ExecutableSnapshot::frame_size(passed_argument_count)` bytes
/// (see `RuntimeOffsets::vm_interpreter_stack_top`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct InlinedFunctionSnapshot {
    pub function: CellId,
    pub formal_parameter_count: u32,
    pub strict: bool,
    /// Whether the function reads its own `this` binding, which an arrow
    /// function does not have. If not, its frame's `this` value is never
    /// observed and may be left empty.
    pub uses_this: bool,
    /// The global `this` value of the function's realm, which non-strict
    /// functions see when called with a null or undefined `this`.
    pub global_this: CellId,
    /// The function's realm, which stays the same for its lifetime.
    pub realm: CellId,
    /// The function's `SharedFunctionInstanceData`, which all closures of
    /// the same function share.
    pub shared_data: CellId,
}

// NB: The bits of the interpreter's feedback below are those of
//     `Libraries/LibJS/Rust/src/layout/feedback.rs`, which
//     `jit/runtime_info.rs` checks.

/// `arith_feedback` bits: the kinds of operands an instruction saw.
pub mod arith_feedback {
    pub const INT32: u8 = 1 << 0;
    pub const DOUBLE: u8 = 1 << 1;
    /// A result that is not an int32 on the path int32 operands take.
    pub const INT32_OVERFLOW: u8 = 1 << 2;
    /// Int32 operands, which `INT32_OVERFLOW` implies.
    pub const SAW_INT32: u8 = INT32 | INT32_OVERFLOW;
    pub const STRING: u8 = 1 << 3;
    pub const BIG_INT: u8 = 1 << 4;
    /// Operands that are no numbers, strings or bigints.
    pub const OTHER: u8 = 1 << 5;
}

/// `call_feedback_flags` bits.
pub mod call_feedback_flags {
    pub const POLYMORPHIC: u8 = 1 << 0;
    /// The calls the callee forwarded went to closures of one function, the
    /// forwarded target being the first.
    pub const FORWARDED_CLOSURES: u8 = 1 << 4;
}

/// `keyed_feedback_bits` bits: the keys and elements a keyed access saw.
pub mod keyed_feedback_bits {
    pub const INT32_INDEX: u32 = 1 << 0;
    pub const STRING_KEY: u32 = 1 << 1;
    pub const SYMBOL_KEY: u32 = 1 << 2;
    pub const OTHER_KEY: u32 = 1 << 3;
    pub const KEY_KINDS_MASK: u32 = 0x1f;
    pub const PACKED: u32 = 1 << 5;
    pub const HOLEY: u32 = 1 << 6;
    pub const OTHER_ELEMENTS: u32 = 1 << 7;
    pub const OUT_OF_BOUNDS: u32 = 1 << 8;
    pub const TYPED_ARRAY_SHIFT: u32 = 9;
}

impl CallFeedbackSnapshot {
    /// The site's single callee and which intrinsic it is, if it is one.
    pub fn monomorphic_intrinsic(&self) -> Option<(CellId, Intrinsic)> {
        if self.flags & call_feedback_flags::POLYMORPHIC != 0 {
            return None;
        }
        Some((self.target?, self.target_intrinsic?))
    }

    /// The site's single callee if it is `Function.prototype.call`, whose
    /// calls JIT code makes by calling the `this` value directly.
    pub fn function_prototype_call_target(&self) -> Option<CellId> {
        if self.flags & call_feedback_flags::POLYMORPHIC != 0
            || self.target_intrinsic != Some(Intrinsic::FunctionPrototypeCall)
        {
            return None;
        }
        self.target
    }
}

impl ExecutableSnapshot {
    /// The bytes an interpreter frame of this executable takes on the
    /// interpreter stack when called with `passed_argument_count` arguments,
    /// with `execution_context_size` = `RuntimeOffsets::execution_context_slots`.
    pub fn frame_size(&self, execution_context_size: u32, passed_argument_count: u32) -> u64 {
        let formal_parameter_count = self.function.map_or(0, |function| function.formal_parameter_count);
        let slots = u64::from(self.layout.registers_and_locals_count)
            + u64::from(self.layout.number_of_constants)
            + u64::from(passed_argument_count.max(formal_parameter_count));
        u64::from(execution_context_size) + 8 * slots
    }
}

/// A copy of the interpreter feedback of one executable, indexed by the
/// feedback slot indices in the bytecode. The bits mean the same as in
/// `Libraries/LibJS/Rust/src/layout/feedback.rs`.
#[derive(Debug, Clone, Default)]
pub struct FeedbackSnapshot {
    /// `ArithFeedback` bits.
    pub arith: Vec<u8>,
    /// `ValueFeedback` bits.
    pub value: Vec<u16>,
    pub call: Vec<CallFeedbackSnapshot>,
    pub keyed: Vec<KeyedFeedbackSnapshot>,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct CallFeedbackSnapshot {
    /// The first callee seen at this site, if it is still alive.
    pub target: Option<CellId>,
    /// `CallFeedback::Flags` bits.
    pub flags: u8,
    /// The index in `Snapshot::executables` of `target`'s executable, if it is
    /// an inlining candidate.
    pub inline_executable: Option<u32>,
    /// `target`, if JIT code can call it directly.
    pub direct_call: Option<DirectCallTarget>,
    /// `target`, if it is a raw native function JIT code can call directly.
    pub native_call: Option<NativeCallTarget>,
    /// How `target` forwarded the calls of this site, if it always forwarded
    /// them the same way to the same function.
    pub forwarded: Option<ForwardedCallSnapshot>,
    /// Which intrinsic `target` is, if it is one the compiler knows.
    pub target_intrinsic: Option<Intrinsic>,
    /// `target`, if JIT code can inline its constructs.
    pub construct: Option<ConstructTarget>,
}

/// A constructor whose construct JIT code can inline: a base constructor
/// without fields, whose function object (of `function_shape`) holds
/// `prototype` as the data property at `prototype_offset`. Its construct
/// creates `this` as an empty plain object of `this_shape`, whose prototype
/// that is, and runs the body of the snapshot executable `executable`
/// (with no function environment).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ConstructTarget {
    pub executable: u32,
    pub function_shape: CellId,
    pub prototype_offset: u32,
    pub prototype: CellId,
    pub this_shape: CellId,
    /// How many properties the new object gets room for (see
    /// `SharedFunctionInstanceData::construct_reserve()`).
    pub reserve: u32,
}

/// How a callee forwards calls to another function.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Forwarding {
    /// `Function.prototype.apply`, with an array-like argument list.
    Apply,
    /// `Function.prototype.call`.
    Call,
    /// A bound function.
    Bound,
    /// A builtin written in JavaScript, which calls its first argument back.
    /// The site's `inline_executable` is the builtin, the forwarded call's
    /// that of the callback.
    Callback,
}

/// A call site whose single callee forwarded every call the same way to the
/// same function (`target`), passing it `argument_count` arguments.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ForwardedCallSnapshot {
    pub forwarding: Forwarding,
    pub target: CellId,
    /// Which intrinsic `target` is, if it is one the compiler knows.
    pub target_intrinsic: Option<Intrinsic>,
    pub argument_count: u32,
    /// The index in `Snapshot::executables` of `target`'s executable, if it
    /// is an inlining candidate.
    pub inline_executable: Option<u32>,
    /// For bound functions: the bound `this` and the first
    /// `bound_argument_count` bound arguments, as NaN-boxed values.
    pub bound_this: u64,
    pub bound_arguments: [u64; 4],
    pub bound_argument_count: u8,
}

/// A call target JIT code can call directly, building its frame like the
/// interpreter's `Call` fast path does: an ECMAScript function that can be
/// called in an inline frame, and that needs no function environment or
/// one JIT code can allocate itself. Its executable and realm stay the same
/// for as long as the function lives. Code that calls it embeds
/// `function.function` (`function.shared_data` for `closures`), `executable`,
/// `function.realm` (and
/// `function.global_this` if it binds `this` to it, and the cells of its
/// environment template) and must list them in
/// `CompiledCode::embedded_cells`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DirectCallTarget {
    pub function: InlinedFunctionSnapshot,
    pub executable: CellId,
    /// The address of the executable's entry in the JIT entry table (see
    /// `DynamicCallLayout::jit_entry_table`), which the call enters.
    pub entry: u64,
    pub registers_and_locals_count: u32,
    /// Where the arguments start in the function's frames.
    pub registers_and_locals_and_constants_count: u32,
    pub function_fields: FunctionFrameFields,
    /// The function environment each call gets, if it needs one: an index
    /// into the calling executable's `ExecutableSnapshot::environment_templates`.
    /// The call's frame has it as its lexical and variable environment.
    pub environment: Option<u32>,
    /// Whether the site called other closures of `function` too: functions
    /// with its code (its `shared_data`), but their own environments, which
    /// the call reads from the callee instead of `function_fields`. Only
    /// targets without an `environment` of their own have closures.
    pub closures: bool,
}

/// A `DeclarativeEnvironment` like the ones a `CreateLexicalEnvironment`
/// without a catch environment creates (see `create_lexical_environment()`)
/// when its shape cache has the environment's final shape, with room for its
/// `binding_count` binding values in its cell (or none). JIT code allocates
/// it from the local free list at `allocator` (cells of `cell_size` bytes,
/// see `ObjectAllocationInfo`) and fills it with `words`, then stores the
/// pointer to its binding values (which follow `words`, if
/// `inline_binding_values`) at `binding_values_offset` and its parent at
/// `outer_offset`. Code that allocates it embeds `shape`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LexicalEnvironmentTemplateSnapshot {
    pub allocator: u64,
    pub cell_size: u32,
    pub words: Vec<u64>,
    pub binding_count: u32,
    pub inline_binding_values: bool,
    pub binding_values_offset: u32,
    pub outer_offset: u32,
    pub shape: Option<CellId>,
}

/// A `FunctionEnvironment` like the ones calls of a function get (see
/// `ECMAScriptFunctionObject::inline_call_environment()`), with its final
/// shape and room for its binding values in the cell, as JIT code allocates
/// it: from the local free list at `allocator` (like
/// `ObjectSizeClass::allocator`), with `words` (the cell's bytes, with a
/// clear mark), then the pointer to its binding values, which start right
/// after `words`, at `binding_values_offset`, and the `this` value the call
/// binds at `this_value_offset` if `binds_this`. The binding values need no
/// initialization: the environment has none yet.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EnvironmentTemplateSnapshot {
    pub allocator: u64,
    pub cell_size: u32,
    pub words: Vec<u64>,
    /// Whether the environment has room for binding values in its cell.
    pub inline_binding_values: bool,
    pub binding_values_offset: u32,
    pub binds_this: bool,
    pub this_value_offset: u32,
    /// The cells `words` refer to besides the function, which code that
    /// allocates the environment embeds: its shape and outer environment.
    pub cells: Vec<CellId>,
}

/// The words of an ECMAScript function's `[[ScriptOrModule]]`,
/// `[[Environment]]` and private environment, which its frames start with
/// (see `RuntimeOffsets::ecmascript_function_script_or_module` and the
/// fields after it). They stay the same for the function's lifetime, and
/// what they point to lives as long as the function.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct FunctionFrameFields {
    pub script_or_module: [u64; 2],
    pub environment: u64,
    pub private_environment: u64,
}

impl FunctionFrameFields {
    /// Whether the functions have the same `[[ScriptOrModule]]`: the tag in
    /// the low byte of the first word (whose other bytes are padding) and
    /// the record in the second.
    pub fn has_script_or_module_of(&self, other: &FunctionFrameFields) -> bool {
        self.script_or_module[0] as u8 == other.script_or_module[0] as u8
            && self.script_or_module[1] == other.script_or_module[1]
    }
}

/// A raw native function that JIT code calls directly, in the lightweight
/// frame the interpreter's call fast path builds for raw native functions.
/// The compiled code must list `function` and `realm` in
/// `CompiledCode::embedded_cells`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NativeCallTarget {
    pub function: CellId,
    /// The function's realm, the frame's realm.
    pub realm: CellId,
    /// `ThrowCompletionOr<Value> (*)(VM&)`: the native function.
    pub entry: u64,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct KeyedFeedbackSnapshot {
    /// `KeyedFeedback::Bits`.
    pub bits: u32,
}

/// Which tier a property lookup cache is in.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum PropertyCacheKind {
    #[default]
    Empty,
    Monomorphic,
    /// Up to four shapes, most recently used first.
    Polymorphic,
    /// Too many shapes. `entries` holds only the most recently used one, or
    /// none for a keyed access that saw too many keys to cache at all.
    Megamorphic,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PropertyCacheSnapshot {
    pub kind: PropertyCacheKind,
    /// The non-empty entries.
    pub entries: Vec<PropertyCacheEntrySnapshot>,
}

/// `PropertyLookupCache::Entry::Type`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PropertyCacheEntryType {
    AddOwnProperty,
    ChangeOwnProperty,
    GetOwnProperty,
    ChangePropertyInPrototypeChain,
    GetPropertyInPrototypeChain,
    GetMissingProperty,
}

/// One `PropertyLookupCache::Entry`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PropertyCacheEntrySnapshot {
    pub entry_type: PropertyCacheEntryType,
    pub property_offset: u32,
    pub shape_dictionary_generation: u32,
    /// Whether `shape` is a dictionary shape, whose dictionary generation
    /// changes when its properties change in place. Only those need their
    /// generation checked. A shape never becomes a dictionary after creation.
    pub shape_is_dictionary: bool,
    /// Whether `shape` was stable (see `Dependency::StableShape`).
    pub shape_is_stable: bool,
    pub writes_data_property: bool,
    pub from_shape: Option<CellId>,
    pub shape: Option<CellId>,
    pub prototype: Option<CellId>,
    pub prototype_chain_validity: Option<CellId>,
    /// Whether `prototype_chain_validity` was still valid, so that code may
    /// depend on it staying valid instead of checking it.
    pub prototype_chain_valid: bool,
    /// The key of the property the entry describes, for the caches of keyed
    /// accesses (`GetByValue`, `PutByValue`), whose entries are each for one
    /// (shape, key) pair: the entry only applies when the access's key operand
    /// is this exact string or symbol cell (keys are matched by identity, never
    /// by contents). `None` for the caches of named accesses (`GetById` and
    /// friends), whose instruction determines the property. Array index keys
    /// never have entries; they use the elements fast paths.
    pub key: Option<CellId>,
    /// `key` as the encoded (NaN-boxed) JS value the access's key operand holds
    /// when the entry applies, or 0 when there is no key. Comparing the operand's
    /// 64 bits against it is the whole key check.
    pub key_value: u64,
    /// For `GetPropertyInPrototypeChain` entries: the object `prototype`
    /// held in the property when the snapshot was taken, if it held one
    /// (such as a method). Nothing invalidates the entry when the property
    /// is assigned another value, so code that relies on it checks it.
    pub prototype_property: Option<CellId>,
    /// Which intrinsic `prototype_property` is, if it is one the compiler
    /// knows.
    pub prototype_property_intrinsic: Option<Intrinsic>,
    /// The function of the accessor the access calls, if it is an ECMAScript
    /// function: for `GetPropertyInPrototypeChain` entries, the getter of the
    /// accessor `prototype` held in the property when the snapshot was
    /// taken, for `ChangePropertyInPrototypeChain` entries its setter, and
    /// for `GetOwnProperty` entries the getter of the accessor the property
    /// held when the interpreter last called it.
    pub accessor_function: Option<AccessorFunctionSnapshot>,
    /// Whether the property held an accessor: for `GetOwnProperty` entries
    /// when the interpreter last called its getter, and for entries of a
    /// prototype when the snapshot was taken. Loads of the property as data
    /// would exit.
    pub holds_accessor: bool,
}

/// The getter or setter of an accessor property that accesses call.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AccessorFunctionSnapshot {
    pub function: CellId,
    /// The index in `Snapshot::executables` of the function's executable,
    /// if it is an inlining candidate.
    pub inline_executable: Option<u32>,
}

/// Built-in functions the compiler recognizes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Intrinsic {
    /// `Function.prototype.apply`.
    FunctionPrototypeApply,
    /// `Function.prototype.call`.
    FunctionPrototypeCall,
    /// `Array.prototype.push`.
    ArrayPrototypePush,
    /// `Array.prototype.slice`.
    ArrayPrototypeSlice,
    /// `Object.prototype.hasOwnProperty`.
    ObjectPrototypeHasOwnProperty,
    /// `String.fromCharCode`.
    StringFromCharCode,
    /// `String`, the constructor, called as a function.
    StringConstructor,
    /// `Array`, the constructor, called as a function.
    ArrayConstructor,
    /// `Object`, the constructor, called as a function.
    ObjectConstructor,
    /// `Boolean`, the constructor, called as a function.
    BooleanConstructor,
}

/// The runtime helpers of intrinsics (the out of line probe of
/// `Op::ProbeHasProperty`), or 0 for those the runtime lacks. Each takes the
/// VM and the node's inputs and returns a value, or the empty value, having
/// done nothing observable, if it cannot do what it was asked to (the probe
/// then misses).
#[derive(Debug, Clone, Copy, Default)]
pub struct IntrinsicHelpers {
    /// `u64 (VM*, u64 object, u64 key)`: `Object.prototype.hasOwnProperty`
    /// called on an object with a string or symbol key, as a boolean.
    pub has_own_property: u64,
    /// `u64 (VM*, u64 key, u64 object)`: `key in object` for a string or
    /// symbol key, as a boolean.
    pub has_property: u64,
}

/// Addresses of runtime helpers, struct field offsets and value encoding
/// constants that generated code needs, as `Libraries/LibJS/Rust/src/jit/runtime_info.rs`
/// fills them in.
///
/// Every address is embedded into the code as an absolute immediate.
#[derive(Debug, Clone, Default)]
pub struct RuntimeInfo {
    /// Whether no object with the `[[IsHTMLDDA]]` internal slot exists yet,
    /// so that code may depend on that (see `Dependency::NoHtmlDdaObjects`).
    pub no_htmldda_objects: bool,
    /// The address of the slow path each generic opcode calls, indexed by
    /// opcode, or 0 if there is none. `codegen::slow_path_symbol()` names the
    /// function for each opcode and the calling convention it is called with.
    pub slow_paths: Vec<u64>,
    /// `i64 libjs_jit_call(VM*, ExecutionContext*, u32 pc)`: runs the call
    /// instruction at `pc` to completion, writes its result into its
    /// destination slot, and returns a slow path control word.
    pub jit_call: u64,
    /// `void libjs_jit_exit(VM*, ExecutionContext*, u32 exit_index, RegisterDump const*)`: the runtime finds the
    /// code through the frame's executable.
    ///
    /// Exits inside inlined code describe every frame, innermost first: the
    /// inlined callees at their pc and each caller `ResumeAfter { dst }` at
    /// its `Call` instruction. The runtime materializes the callee frames,
    /// linked to their callers like interpreter inline calls (the passed
    /// argument count and return pc come from the `Call` instruction), and
    /// the interpreter continues in the innermost frame.
    pub jit_exit: u64,
    /// How JIT code allocates plain objects itself; see `ObjectAllocationInfo`.
    pub object_allocation: ObjectAllocationInfo,
    /// How JIT code allocates rope strings itself; see `RopeAllocationInfo`.
    pub rope_allocation: RopeAllocationInfo,
    /// How JIT code allocates arrays itself; see `ArrayAllocationInfo`.
    pub array_allocation: ArrayAllocationInfo,
    /// How JIT code allocates closures itself; see `FunctionAllocationInfo`.
    pub function_allocation: FunctionAllocationInfo,
    /// `Environment* libjs_jit_create_lexical_environment(VM*, Environment* parent, Executable*, u32 shape_cache,
    /// u32 capacity)`: what `CreateLexicalEnvironment` (without a catch
    /// environment) creates, for JIT code that cannot allocate it itself
    /// (see `LexicalEnvironmentTemplateSnapshot`), or 0.
    pub create_lexical_environment: u64,
    /// `u64 asm_helper_to_boolean(u64 encoded_value)`, returning 0 or 1.
    pub to_boolean: u64,
    /// `u64 libjs_jit_primitive_to_string(VM*, u64 primitive)`:
    /// `String(primitive)`.
    pub primitive_to_string: u64,
    /// `u64 libjs_jit_to_object(VM*, u64 primitive, u64 function)`: ToObject
    /// of a primitive other than undefined and null, as the builtin
    /// `function` makes it, or the empty value if that function is of
    /// another realm.
    pub to_object: u64,
    /// `u64 libjs_jit_array_create(VM*, u32 length, u64 function)`:
    /// ArrayCreate(length) as the builtin `function` (the Array constructor)
    /// makes it, or the empty value if that function is of another realm.
    pub array_create: u64,
    /// The helpers of intrinsics; see `IntrinsicHelpers`.
    pub intrinsic_helpers: IntrinsicHelpers,
    /// `u64 libjs_jit_create_arguments(VM*, ExecutionContext* frame, u32 mapped)`:
    /// creates the arguments object of `frame` from its arguments like
    /// `CreateArguments` of the given kind does, and returns it as a value.
    pub create_arguments: u64,
    /// `i64 libjs_jit_call_forwarding_arguments(VM*, ExecutionContext* frame, u32 pc)`:
    /// runs the `Call` instruction at `pc` of `frame`, `f.apply(this_arg, arguments)`
    /// whose callee is `Function.prototype.apply` and whose last argument is
    /// the frame's arguments object, which the compiled code never created:
    /// calls `f` with `this_arg` and the frame's passed arguments, and
    /// returns a slow path control word like `jit_call`.
    pub call_forwarding_arguments: u64,
    /// `i64 libjs_jit_finish_direct_call(VM*, ExecutionContext* frame, u32 pc,
    /// u64 status)`: finishes a direct call (see
    /// `DirectCallTarget`) made for the `Call` instruction at `pc` of `frame`
    /// that did not return to JIT code: `status` is the `JitStatus` the
    /// callee's JIT code returned (`Resume` or `ExitInterpreter`), or
    /// `Returned` if JIT code did not enter the callee's frame, which is the
    /// running execution context. Runs the callee's frame to completion in the
    /// interpreter as needed, and returns a slow path control word for the
    /// call, whose result is in its destination slot.
    pub finish_direct_call: u64,
    /// `i64 asm_helper_handle_raw_native_exception(VM*, u64 exception)`: unwinds
    /// the frame of a raw native function that threw, and returns a slow
    /// path control word for the exception.
    pub raw_native_exception: u64,
    /// `ExecutionContext* libjs_jit_push_inlined_call_frames(VM*,
    /// ExecutionContext* frame, u32 site, u64 frame_pointer)`: pushes the
    /// frames of the inlined calls of the `SiteKind::Call` `site` of
    /// the code running `frame`, from the JIT frame at `frame_pointer`, and
    /// returns the innermost one, which runs.
    pub push_inlined_call_frames: u64,
    /// `i64 libjs_jit_finish_inlined_direct_call(VM*, ExecutionContext*
    /// frame, u32 site, u64 frame_pointer, u64 status)`: like
    /// `finish_direct_call` for a direct call at an inlined call `site`,
    /// once it materialized the frames of its inlined calls below the
    /// callee's frame.
    pub finish_inlined_direct_call: u64,
    /// `i64 libjs_jit_inlined_raw_native_exception(VM*, ExecutionContext*
    /// frame, u32 site, u64 frame_pointer, u64 exception)`: like
    /// `raw_native_exception` for a native call at an inlined call `site`,
    /// once it materialized the frames of its inlined calls below the
    /// native function's frame.
    pub inlined_raw_native_exception: u64,
    /// `u64 libjs_jit_array_push(Array*, u64 value)`: appends `value` to the
    /// array, which `Array.prototype.push` would append it to without any
    /// observable step, growing its packed elements, and returns the new
    /// length as a value.
    pub array_push: u64,
    /// `u64 libjs_jit_slice_arguments(VM*, ExecutionContext* frame, i32 start)`:
    /// `Array.prototype.slice.call(arguments, start)` with the arguments
    /// object of `frame`, which the code never created, as an array value.
    pub slice_arguments: u64,
    /// `%Array.prototype%` and `%Object.prototype%` of the realm of the
    /// compiled code. Code that embeds them must list them in
    /// `CompiledCode::embedded_cells`.
    pub array_prototype: CellId,
    pub object_prototype: CellId,
    /// `ExecutionContext::no_yield_continuation`.
    pub no_yield_continuation: u32,
    /// `js_heap_region_base`: cell pointers are this plus the offset bits of a cell value.
    pub heap_region_base: u64,
    /// `GC::HEAP_REGION_OFFSET_MASK`.
    pub heap_region_offset_mask: u64,
    /// `GC::SHIFTED_IS_CELL_PATTERN`: the tag bits of a boxed environment.
    pub shifted_is_cell_pattern: u64,
    /// `Object::Flag::IsFunction`.
    pub object_flag_is_function: u16,
    pub offsets: RuntimeOffsets,
    pub layout: RuntimeLayout,
    pub dynamic_calls: DynamicCallLayout,
}

/// What generic calls need to call any ECMAScript function the way the
/// interpreter's `Call` fast path does, from the function object at call
/// time. JIT code makes no such calls if `object_flag_is_ecmascript_function`
/// is 0 (the default).
#[derive(Debug, Clone, Copy, Default)]
pub struct DynamicCallLayout {
    /// `Object::Flag::IsECMAScriptFunctionObject`.
    pub object_flag_is_ecmascript_function: u16,
    /// `EcmascriptFunctionObject::shared_data` (pointer).
    pub ecmascript_function_shared_data: u32,
    /// `SharedFunctionInstanceData::executable` (pointer).
    pub shared_data_executable: u32,
    /// `SharedFunctionInstanceData::asm_call_metadata` (u64): the formal
    /// parameter count in the low 32 bits, and the flags below.
    pub shared_data_asm_call_metadata: u32,
    /// `Executable::jit_entry_slot` (u32): the executable's slot in the JIT
    /// entry table.
    pub executable_jit_entry_slot: u32,
    /// The address of the JIT entry table's entries: what calls of the
    /// executable of each slot enter its frames with, never null. Without
    /// JIT code, it is an entry that makes the frame the running one and
    /// returns `JitStatus::Resume`, so that the interpreter runs it. Slot 0
    /// has that entry and belongs to no executable.
    pub jit_entry_table: u64,
    /// Slots are masked with this, which keeps them inside the table.
    pub jit_entry_slot_mask: u32,
    /// Where the table keeps the address of each slot's executable, as a
    /// byte offset from its entries.
    pub jit_entry_table_owners: u32,
    pub metadata_can_inline_call: u64,
    pub metadata_needs_environment_or_this_value_resolution: u64,
    pub metadata_uses_this: u64,
    pub metadata_strict: u64,
    /// `Executable::registers_and_locals_and_constants_count` (u32).
    pub executable_registers_and_locals_and_constants_count: u32,
    /// `Shape::realm` (pointer).
    pub shape_realm: u32,
    /// `Object::Flag::IsRawNativeFunction`, or 0 if JIT code makes no
    /// dynamic calls of raw native functions.
    pub object_flag_is_raw_native_function: u16,
    /// `RawNativeFunction::native_function_index` (u32).
    pub raw_native_function_index: u32,
    /// `VmHead::native_function_table_data` (pointer to `NativeFunctionTableEntry`).
    pub vm_native_function_table: u32,
    /// `NATIVE_FUNCTION_TABLE_INDEX_MASK`: native function indices are masked with it, which keeps them inside the
    /// table.
    pub native_function_table_index_mask: u32,
    /// `sizeof(NativeFunctionTableEntry)`, a power of two.
    pub native_function_table_entry_size: u32,
    /// `NativeFunctionTableEntry::function`: the raw native function.
    pub native_function_table_entry_function: u32,
    /// `CallEnvironment libjs_jit_prepare_call_environment(VM*, ECMAScriptFunctionObject*, u64 this_argument)`,
    /// or 0 if JIT code makes no dynamic calls of functions that need a
    /// function environment or the resolution of their this value: returns
    /// the callee frame's environment (null to take the generic path) and
    /// this value.
    pub prepare_call_environment: u64,
    /// `SharedFunctionInstanceData::call_environment_template` (pointer to
    /// a `CallEnvironmentTemplate` cell, null until the runtime makes one,
    /// decoded into the heap region like a cell value), or 0
    /// if call stubs leave every function environment to
    /// `prepare_call_environment`. The stub allocates the function
    /// environment of a call itself from the callee's template, the way
    /// `ObjectAllocationInfo` describes (with the heap from
    /// `RuntimeInfo::object_allocation`): it fills the cell with the
    /// template's words, then stores the pointer to its binding values (the
    /// cell's address plus the template's binding values offset, unless that
    /// is 0), the callee's environment as its outer environment, the callee
    /// as its function object and, if the template binds `this`, the call's
    /// `this` value, which must need no conversion.
    pub shared_data_call_environment_template: u32,
    /// The fields of a `CallEnvironmentTemplate`: the size class (u64), the
    /// cell size (u64), the binding values offset (u64), whether calls bind
    /// `this` (u64, 0 or 1), and the words of the environment.
    pub call_environment_template_size_class: u32,
    pub call_environment_template_cell_size: u32,
    pub call_environment_template_binding_values_offset: u32,
    pub call_environment_template_binds_this: u32,
    pub call_environment_template_words: u32,
    /// The address of a table of the addresses of the local free lists of
    /// the function environment size classes, which the stub pops the cell
    /// of a template's size class from, masked with
    /// `function_environment_size_class_mask` into the table.
    pub function_environment_free_lists: u64,
    pub function_environment_size_class_mask: u32,
    /// The number of words of a `FunctionEnvironment`.
    pub function_environment_words: u32,
    /// `FunctionEnvironment` fields: the pointer to its binding values, its
    /// outer environment, its function object and its this value.
    pub function_environment_binding_values: u32,
    pub function_environment_outer: u32,
    pub function_environment_function_object: u32,
    pub function_environment_this_value: u32,
    /// The address of the call stub (see `codegen::generate_call_stub()`),
    /// or 0 if JIT code makes every call it does not make directly through
    /// `RuntimeInfo::jit_call`.
    pub call_stub: u64,
}

/// The allocation contract for plain objects (`JS::Object` created with a
/// shape, as `NewObject` does).
///
/// Plain objects come in size classes by how many named property values they
/// hold inline, right in their cell. An object is created in the first size
/// class whose `inline_capacity` is at least the property count of its shape,
/// or `empty_object_inline_capacity` for a shape without properties (see
/// `size_class_for()`). JIT code can only allocate objects that fit a size
/// class itself.
///
/// Fast path, entirely inline, with no call, with the size class's
/// `allocator`, `cell_size` and `inline_capacity`:
/// 1. If `size_classes` is empty, there is no fast path (sanitizer builds and
///    GC stress testing allocate every cell through the heap).
/// 2. Let `allocated` = the u64 at `heap + heap_allocated_bytes_offset`. If
///    `allocated + cell_size` > the u64 at `heap + heap_threshold_offset`,
///    take the slow path (the heap collects garbage there).
/// 3. Let `cell` = the pointer at `allocator + local_free_list_offset`. If
///    `cell & freelist_link_mask` is 0, the list is empty: take the slow
///    path (it refills the list).
/// 4. Let `link` = the u64 at `cell + freelist_next_offset`. Store
///    `(cell & !freelist_link_mask) | (link & freelist_link_mask)`, the next
///    cell, which is in the block of `cell` whatever `link` is (cells are in
///    the heap region, where anything may have been corrupted), to
///    `allocator + local_free_list_offset`, `allocated + cell_size` to
///    `heap + heap_allocated_bytes_offset`, and add `cell_size` to the u64 at
///    `heap + heap_total_allocated_bytes_offset`.
/// 5. Copy `template` (`8 * template.len()` bytes, the start of every cell) to
///    `cell`. Then store `inline_capacity` (a u8) at
///    `cell + inline_capacity_offset`, the shape at `cell + shape_offset`,
///    `cell + inline_storage_offset` (the address) at
///    `cell + named_properties_offset`, and at
///    `cell + inline_storage_offset + 8 * index` the value of each named
///    property (in shape order; NaN-boxed `undefined` for the ones not
///    initialized yet) followed by NaN-boxed `undefined` up to
///    `inline_capacity` values.
///
/// No garbage can be collected between steps 2 and 5, and the cell needs no
/// marking: local free lists only ever hold cells of blocks that the
/// incremental sweep is done with.
///
/// Slow path: `Object* libjs_jit_allocate_object(VM*, Shape*, u32 reserve)`
/// returns a complete object of that shape with every named property
/// `undefined` (for any property count), with room for `reserve` properties
/// inline if a size class has room for them. It may collect garbage, like
/// any call.
#[derive(Debug, Clone, Default)]
pub struct ObjectAllocationInfo {
    /// By ascending `inline_capacity`.
    pub size_classes: Vec<ObjectSizeClass>,
    pub empty_object_inline_capacity: u32,
    pub heap: u64,
    pub heap_allocated_bytes_offset: u32,
    pub heap_threshold_offset: u32,
    pub heap_total_allocated_bytes_offset: u32,
    pub local_free_list_offset: u32,
    pub freelist_next_offset: u32,
    pub freelist_link_mask: u32,
    /// The bytes every new plain object starts with, as u64 words.
    pub template: Vec<u64>,
    pub shape_offset: u32,
    pub named_properties_offset: u32,
    pub inline_storage_offset: u32,
    /// `Object::inline_named_capacity` (u8).
    pub inline_capacity_offset: u32,
    /// `Object* libjs_jit_allocate_object(VM*, Shape*, u32 reserve)`.
    pub slow_path: u64,
}

impl ObjectAllocationInfo {
    /// The size class JIT code allocates plain objects whose shape has
    /// `property_count` properties in, or `None` if it must take the slow path.
    pub fn size_class_for(&self, property_count: u32) -> Option<&ObjectSizeClass> {
        let inline_values = if property_count == 0 {
            self.empty_object_inline_capacity
        } else {
            property_count
        };
        self.size_classes
            .iter()
            .find(|size_class| size_class.inline_capacity >= inline_values)
    }
}

/// The allocation contract for rope strings (`JS::RopeString`, the
/// concatenation of two strings), with the heap fields of
/// `ObjectAllocationInfo`.
///
/// Concatenating two non-empty strings of at least `min_length` code units
/// together (and fewer than `u32::MAX`) makes a rope string. JIT code
/// allocates its cell like a plain object's (steps 1 to 4 of
/// `ObjectAllocationInfo`, with `allocator` and `cell_size`), copies
/// `template` (`8 * template.len()` bytes) to it, and stores the length
/// (u32) at `cell + length_offset` and the halves (cell pointers) at
/// `cell + lhs_offset` and `cell + rhs_offset`. If `allocator` is 0, there is
/// no fast path. The slow path is the interpreter's.
#[derive(Debug, Clone, Default)]
pub struct RopeAllocationInfo {
    /// The `GC::CellAllocator` of rope strings.
    pub allocator: u64,
    pub cell_size: u32,
    /// The bytes every new rope string starts with, as u64 words.
    pub template: Vec<u64>,
    pub lhs_offset: u32,
    pub rhs_offset: u32,
    pub length_offset: u32,
    pub min_length: u32,
}

/// The allocation contract for arrays with packed elements (`JS::Array`
/// of the compiled code's realm, with `%Array.prototype%`), with the heap
/// fields of `ObjectAllocationInfo`.
///
/// An array of `count` elements takes an array cell (`allocator`,
/// `cell_size`) and, unless `count` is 0, a storage cell of the first
/// storage size class with a `capacity` of at least `count`. JIT code only
/// allocates both itself if both local free lists have a cell: it checks
/// the heap's threshold for the array cell (storage cells never start a
/// collection, nor count towards one), pops both cells, adds both cell
/// sizes to the heap's total allocated bytes and the array's to its
/// allocated bytes since the last collection. Then:
/// 1. Copy `template` to the array cell and store
///    `array + inline_storage_offset` at `array + named_properties_offset`
///    (see `ObjectAllocationInfo`).
/// 2. For storage, copy `storage_template` to the storage cell, store the
///    capacity (u32) at `storage + storage_capacity_offset` and the element
///    values from `storage + storage_values_offset` on, the empty value
///    after the `count` elements, then store the address of the first value
///    at `array + RuntimeLayout::object_indexed_elements`,
///    `indexed_storage_kind_packed` (u8) at `object_indexed_storage_kind`
///    and `count` (u32) at `object_indexed_array_like_size`.
///
/// The template refers to `shape`, which code that allocates arrays embeds.
/// If `allocator` is 0, there is no fast path. Slow path:
/// `Array* libjs_jit_allocate_array(VM*, u32 count)` returns an array of
/// `count` packed `undefined` elements.
#[derive(Debug, Clone, Default)]
pub struct ArrayAllocationInfo {
    pub allocator: u64,
    pub cell_size: u32,
    pub template: Vec<u64>,
    pub shape: CellId,
    /// By ascending capacity.
    pub storage_size_classes: Vec<StorageSizeClass>,
    pub storage_template: Vec<u64>,
    pub storage_values_offset: u32,
    pub storage_capacity_offset: u32,
    pub slow_path: u64,
}

/// The allocation contract for the function objects of closures
/// (`NewFunction` of a plain function without a home object), with the heap
/// fields of `ObjectAllocationInfo`.
///
/// JIT code allocates the cell like a plain object's (steps 1 to 4 of
/// `ObjectAllocationInfo`, with `allocator` and `cell_size`), copies the
/// closure template's `words` to it, and stores
/// `cell + ObjectAllocationInfo::inline_storage_offset` at
/// `cell + ObjectAllocationInfo::named_properties_offset`, and the lexical
/// and private environments of the frame (cell pointers) at
/// `RuntimeOffsets::ecmascript_function_environment` and
/// `ecmascript_function_private_environment`. If `allocator` is 0, there is
/// no fast path. Slow path:
/// `ECMAScriptFunctionObject* libjs_jit_clone_function(VM*, ECMAScriptFunctionObject* sample, Environment*, PrivateEnvironment*)`.
#[derive(Debug, Clone, Copy, Default)]
pub struct FunctionAllocationInfo {
    pub allocator: u64,
    pub cell_size: u32,
    pub slow_path: u64,
}

/// A size class of element storage (see `ArrayAllocationInfo`).
#[derive(Debug, Clone, Copy, Default)]
pub struct StorageSizeClass {
    pub allocator: u64,
    pub cell_size: u32,
    pub capacity: u32,
}

/// A size class of plain objects (see `ObjectAllocationInfo`).
#[derive(Debug, Clone, Copy, Default)]
pub struct ObjectSizeClass {
    /// The `GC::CellAllocator` of the size class.
    pub allocator: u64,
    pub cell_size: u32,
    /// How many named property values objects of this size class hold inline.
    pub inline_capacity: u32,
}

/// Byte offsets of the fields generated code accesses, as
/// `Libraries/LibJS/Rust/src/jit/runtime_info.rs` takes them from the runtime's
/// layout types.
#[derive(Debug, Clone, Copy, Default)]
pub struct RuntimeOffsets {
    /// `ExecutionContext::program_counter` (u32).
    pub execution_context_program_counter: u32,
    /// `ExecutionContext::lexical_environment` (cell pointer).
    pub execution_context_lexical_environment: u32,
    /// `ExecutionContext::private_environment` (cell pointer).
    pub execution_context_private_environment: u32,
    /// `ExecutionContext::frame_initialized` (bool).
    pub execution_context_frame_initialized: u32,
    /// `ExecutionContext::executable` (cell pointer).
    pub execution_context_executable: u32,
    /// `sizeof(ExecutionContext)`: where the slots start.
    pub execution_context_slots: u32,
    /// `VmHead::running_execution_context`.
    pub vm_running_execution_context: u32,
    /// `Vm::jit_native_stack_limit` (u64): JIT code exits at entry if the
    /// stack pointer is below it, and its calls take their slow paths,
    /// which run the callee in the interpreter.
    pub vm_jit_native_stack_limit: u32,
    /// `PrivateEnvironment::outer`.
    pub private_environment_outer: u32,
    /// `Object::flags` (u16).
    pub object_flags: u32,
    /// `VM_INTERPRETER_STACK_TOP` and `VM_INTERPRETER_STACK_LIMIT`: the
    /// interpreter stack's next free byte and its end (pointers). JIT code
    /// that may materialize inlined frames checks at entry that they fit
    /// between the two, and exits to the interpreter otherwise.
    pub vm_interpreter_stack_top: u32,
    pub vm_interpreter_stack_limit: u32,
    /// `Object::shape` (cell pointer).
    pub object_shape: u32,
    /// `Object::named_properties`: a pointer to the object's named property
    /// values, one `Value` per property offset, wherever they are stored.
    pub object_named_properties: u32,
    /// `Shape::dictionary_generation` (u32).
    pub shape_dictionary_generation: u32,
    /// `PrototypeChainValidity::valid` (bool).
    pub prototype_chain_validity_valid: u32,
    /// `Accessor::getter` and `Accessor::setter` (cell pointers, null if
    /// none).
    pub accessor_getter: u32,
    pub accessor_setter: u32,
    /// `ExecutionContext::function` (cell pointer).
    pub execution_context_function: u32,
    /// `ExecutionContext::realm` (cell pointer).
    pub execution_context_realm: u32,
    /// `ExecutionContext::script_or_module` (16 bytes).
    pub execution_context_script_or_module: u32,
    /// `ExecutionContext::variable_environment` (cell pointer).
    pub execution_context_variable_environment: u32,
    /// `ExecutionContext::frame_id` (u64).
    pub execution_context_frame_id: u32,
    /// `ExecutionContext::skip_when_determining_incumbent_counter` (u32).
    pub execution_context_skip_when_determining_incumbent_counter: u32,
    /// `ExecutionContext::yield_continuation` (u32).
    pub execution_context_yield_continuation: u32,
    /// `ExecutionContext::yield_is_await` (bool).
    pub execution_context_yield_is_await: u32,
    /// `ExecutionContext::yield_value_is_iterator_result` (bool).
    pub execution_context_yield_value_is_iterator_result: u32,
    /// `ExecutionContext::caller_is_construct` (bool).
    pub execution_context_caller_is_construct: u32,
    /// `ExecutionContext::this_value` (a value, empty if absent).
    pub execution_context_this_value: u32,
    /// `ExecutionContext::caller_frame` (pointer).
    pub execution_context_caller_frame: u32,
    /// `ExecutionContext::passed_argument_count` (u32).
    pub execution_context_passed_argument_count: u32,
    /// `ExecutionContext::caller_return_pc` (u32).
    pub execution_context_caller_return_pc: u32,
    /// `ExecutionContext::caller_dst_raw` (u32).
    pub execution_context_caller_dst_raw: u32,
    /// `ExecutionContext::registers_and_constants_and_locals_and_arguments_count` (u32).
    pub execution_context_slot_count: u32,
    /// `ExecutionContext::argument_count` (u32).
    pub execution_context_argument_count: u32,
    /// `ExecutionContext::returns_to_native_caller` (bool).
    pub execution_context_returns_to_native_caller: u32,
    /// `ExecutionContext::runs_jit_code` (bool).
    pub execution_context_runs_jit_code: u32,
    /// `EcmascriptFunctionObject::environment` (cell pointer).
    pub ecmascript_function_environment: u32,
    /// `EcmascriptFunctionObject::private_environment` (cell pointer).
    pub ecmascript_function_private_environment: u32,
    /// `EcmascriptFunctionObject::script_or_module` (16 bytes, as in `ExecutionContext`).
    pub ecmascript_function_script_or_module: u32,
    /// `VmHead::execution_generation` (u32).
    pub vm_execution_generation: u32,
}

/// How much one compile job inlines, in bytecode instructions of the callees.
/// The default inlines nothing.
#[derive(Debug, Clone, Copy, Default)]
pub struct InliningLimits {
    /// Callees of at most this many instructions are always inlined.
    pub always_inlined_instructions: u32,
    /// Otherwise, callees of at most this many instructions are inlined...
    pub max_instructions: u32,
    /// ...as long as all inlined callees together have at most this many.
    pub budget_instructions: u32,
    /// Constructors of at most this many instructions are inlined into
    /// their constructs whatever the budget says: constructs that are not
    /// inlined go through the generic construct path, which costs more than
    /// most constructors that only initialize the properties of `this`.
    pub max_construct_instructions: u32,
    /// Inlined calls nest at most this deep.
    pub max_depth: u32,
}

/// Options of one compile job.
#[derive(Debug, Clone, Copy, Default)]
pub struct CompileOptions {
    /// Return the IR and the register allocation as text in `CompiledCode::dump`.
    pub dump_ir: bool,
    /// Return the IR after building it and after every pass as text in
    /// `CompiledCode::dump`, each headed by "=== after <pass>".
    pub dump_passes: bool,
    /// Return the machine code, annotated with the IR, as text in `CompiledCode::dump`.
    pub dump_asm: bool,
    /// Check the invariants of the IR between passes, also in release builds.
    pub verify_ir: bool,
    /// The loop back edge whose tier-up budget ran out, if a loop (rather
    /// than calls) triggered the compile. Only it gets an on-stack
    /// replacement entry: such an entry merges into the loop, which costs
    /// every iteration.
    pub osr_pc: Option<u32>,
    pub inlining: InliningLimits,
    pub stress: StressOptions,
    /// Return what the code contains in `CompiledCode::coverage`.
    pub coverage: bool,
}

/// Options that make compiled code take its rare paths often, for testing
/// the compiler. All of them are off by default.
#[derive(Debug, Clone, Copy, Default)]
pub struct StressOptions {
    /// The address of a u32 that compiled code counts down before every
    /// node that can exit, taking the node's exit when it reaches 0 (the
    /// runtime then sets it again), or 0 for none. Every node may exit at
    /// its start, before it has any effect, so these exits are always
    /// correct; they show whether the frame states are.
    pub exit_countdown: u64,
    /// Every loop back edge that ran gets an on-stack replacement entry,
    /// not only `CompileOptions::osr_pc`.
    pub osr_at_every_loop: bool,
    /// The register allocator assigns values only to the registers calls
    /// take their arguments and return their results in and two more, so
    /// that values get spilled and reloaded far more often.
    pub few_registers: bool,
}

/// The input of one compile job.
#[derive(Debug, Clone, Default)]
pub struct Snapshot {
    /// Index 0 is the function being compiled; the rest are inlining candidates.
    pub executables: Vec<ExecutableSnapshot>,
    pub runtime: RuntimeInfo,
    pub options: CompileOptions,
}

/// The VM's strings of the results of `typeof`, as values.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct TypeofStrings {
    pub number: u64,
    pub undefined: u64,
    pub object: u64,
    pub string: u64,
    pub symbol: u64,
    pub boolean: u64,
    pub bigint: u64,
    pub function: u64,
}

/// What the inline code of compiled nodes needs to know about the runtime's
/// data structures beyond `RuntimeOffsets`: offsets of their fields (byte
/// offsets into the named objects), the values of their flags and kinds,
/// and the addresses of the VM's caches, as `jit/runtime_info.rs` fills
/// them in.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct RuntimeLayout {
    /// `Object::indexed_elements`: a pointer to the first `Value` of the
    /// indexed elements buffer, or null.
    pub object_indexed_elements: u32,
    /// `Object::indexed_storage_kind` (u8).
    pub object_indexed_storage_kind: u32,
    /// `Object::indexed_array_like_size` (u32).
    pub object_indexed_array_like_size: u32,
    /// `INDEXED_ELEMENTS_CAPACITY`: where the indexed elements buffer keeps
    /// its capacity (u32), relative to the pointer to its first element.
    pub indexed_elements_capacity: i32,
    /// `IndexedStorageKind::None`, `IndexedStorageKind::Packed` and
    /// `IndexedStorageKind::Holey`.
    pub indexed_storage_kind_none: u8,
    pub indexed_storage_kind_packed: u8,
    pub indexed_storage_kind_holey: u8,
    /// `Object::Flag::IsExtensible`.
    pub object_flag_is_extensible: u16,
    /// `Object::Flag::HasMagicalLengthProperty`, which only arrays have.
    pub object_flag_has_magical_length: u16,
    /// `Object::Flag::MayInterfereWithIndexedPropertyAccess`.
    pub object_flag_may_interfere: u16,
    /// `Array::length_writable` and `Array::is_proxy_target` (bools).
    pub array_length_writable: u32,
    pub array_is_proxy_target: u32,
    /// `Shape::prototype`.
    pub shape_prototype: u32,
    /// `Object::Flag::IsTypedArray`.
    pub object_flag_is_typed_array: u16,
    /// `Object::Flag::IsHTMLDDA`.
    pub object_flag_is_htmldda: u16,
    /// `Object::Flag::RequiresSlowAddOwnProperty`.
    pub object_flag_requires_slow_add_own_property: u16,
    /// The capacity (u32) of heap named property storage, relative to the
    /// pointer to its first property.
    pub named_properties_capacity: i32,
    /// `Shape::property_count` (u32).
    pub shape_property_count: u32,
    /// `TypedArrayBase::cached_data_offset` (u64): the offset of the
    /// element data in the primitive storage cage, or
    /// `typed_array_cached_data_offset_invalid`.
    pub typed_array_cached_data_offset: u32,
    pub typed_array_cached_data_offset_invalid: u64,
    /// `TypedArrayBase::array_length.length`, as a u32.
    pub typed_array_array_length: u32,
    /// `TypedArrayBase::kind` (u8).
    pub typed_array_kind: u32,
    /// `TypedArrayBase::Kind` values.
    pub typed_array_kind_uint8: u8,
    pub typed_array_kind_uint8_clamped: u8,
    pub typed_array_kind_uint16: u8,
    pub typed_array_kind_uint32: u8,
    pub typed_array_kind_int8: u8,
    pub typed_array_kind_int16: u8,
    pub typed_array_kind_int32: u8,
    pub typed_array_kind_float32: u8,
    pub typed_array_kind_float64: u8,
    /// `VmHead::primitive_storage_cage_base` (u64). Typed array elements are
    /// at `cage_base + ((cached_data_offset + byte_index) & cage_offset_mask)`.
    pub vm_primitive_storage_cage_base: u32,
    /// `GC::PrimitiveStorage::cage_offset_mask`.
    pub primitive_storage_cage_offset_mask: u64,
    /// `PrimitiveString::length_in_utf16_code_units` (u32).
    pub primitive_string_length: u32,
    /// `PrimitiveString::utf16_string`: the word of the string's storage,
    /// once it has one, which is the identity of a fly string.
    pub primitive_string_storage: u32,
    /// The byte of `PrimitiveString` that holds its interned flag, and the flag's bit
    /// in it: a string is interned if `byte & primitive_string_interned_mask` is not 0.
    pub primitive_string_is_interned: u32,
    pub primitive_string_interned_mask: u8,
    /// The bits of the same byte that hold the kind of a deferred string
    /// (a rope, a substring or an inline string), which has no storage yet:
    /// 0 once resolved.
    pub primitive_string_deferred_kind_mask: u8,
    /// The deferred kind of inline strings, which keep their ASCII bytes in
    /// the cell, at `primitive_string_inline_storage`.
    pub primitive_string_deferred_kind_inline: u8,
    pub primitive_string_inline_storage: u32,
    /// Short `Utf16String`s keep their ASCII bytes in the storage word: its
    /// lowest byte holds `utf16_short_string_flag` and the byte count,
    /// shifted left by `utf16_short_string_byte_count_shift`, and the bytes
    /// follow. Other storage words point at a `Utf16StringData`.
    pub utf16_short_string_flag: u8,
    pub utf16_short_string_byte_count_shift: u8,
    /// `Utf16StringDataHeader::flags` (u32), and its flag for storage of UTF-16
    /// code units rather than ASCII bytes.
    pub utf16_string_data_flags: u32,
    pub utf16_string_data_has_utf16_storage: u32,
    /// Where the code units of a `Utf16StringData` start.
    pub utf16_string_data_storage: u32,
    /// The address of the VM's array of the strings of the 128 ASCII
    /// characters (cell pointers), or 0 for code without one.
    pub single_ascii_character_strings: u64,
    /// `FunctionObject::builtin` (u8), the builtin a function is, if
    /// `FunctionObject::has_builtin` (bool).
    pub function_object_builtin: u32,
    pub function_object_has_builtin: u32,
    /// `Builtin` values.
    pub builtin_string_prototype_char_code_at: u8,
    pub builtin_string_prototype_char_at: u8,
    /// `JS::Builtin::MathAbs`, `MathFloor`, `MathCeil`, `MathRound` and
    /// `MathSqrt`.
    pub builtin_math_abs: u8,
    pub builtin_math_floor: u8,
    pub builtin_math_ceil: u8,
    pub builtin_math_round: u8,
    pub builtin_math_sqrt: u8,
    /// The strings `typeof` results in, as values, or all 0 for code
    /// without them.
    pub typeof_strings: TypeofStrings,
    /// The address of the VM's cache of strings with fly string storage
    /// (cell pointers, or null), or 0 for code without one. The string with
    /// the storage word `w` is at index `u64_hash(w) & fly_string_cache_mask`
    /// if it is there, where `u64_hash` is the MurmurHash3 64-bit finalizer.
    pub fly_string_cache: u64,
    pub fly_string_cache_mask: u32,
    /// The address of the VM's strings of the integers below
    /// `numeric_string_cache_size` (cell pointers, or null where it made
    /// none yet), or 0 for code without them.
    pub numeric_string_cache: u64,
    pub numeric_string_cache_size: u32,
    /// `Environment::outer` (cell pointer).
    pub environment_outer: u32,
    /// `Environment::declarative` (bool): whether the environment is a
    /// `DeclarativeEnvironment` or extends it.
    pub environment_declarative: u32,
    /// The class (pointer) of `ModuleEnvironment`s, which starts their
    /// cells.
    pub module_environment_class: u64,
    /// `DeclarativeEnvironment::binding_values.size` (u64).
    pub declarative_environment_binding_values_size: u32,
    /// The capacity (u64) of `DeclarativeEnvironment::binding_values`.
    pub declarative_environment_binding_values_capacity: u32,
    /// The pointer to the first binding name (a `Utf16FlyString`, one
    /// word) of an `EnvironmentShape`.
    pub environment_shape_binding_names: u32,
    /// `EnvironmentShape::has_unique_binding_names` (bool).
    pub environment_shape_has_unique_binding_names: u32,
    /// `DeclarativeEnvironment::binding_values.data`, the pointer to the first `Value`.
    pub declarative_environment_binding_values: u32,
    /// `DeclarativeEnvironment::shape` (cell pointer, may be null).
    pub declarative_environment_shape: u32,
    /// `DeclarativeEnvironment::rare_data` (pointer).
    pub declarative_environment_rare_data: u32,
    /// `DeclarativeEnvironmentRareData::binding_flags.data`, the pointer to the first flag byte.
    pub rare_data_binding_flags: u32,
    /// `EnvironmentShape::binding_flags.size` (u64).
    pub environment_shape_binding_flags_size: u32,
    /// `EnvironmentShape::binding_flags.data`, the pointer to the first flag byte.
    pub environment_shape_binding_flags: u32,
    /// `DeclarativeEnvironment::serial_number` (u64).
    pub declarative_environment_serial: u32,
    /// `Realm::global_object` (cell pointer).
    pub realm_global_object: u32,
    /// `Realm::global_declarative_environment` (cell pointer).
    pub realm_global_declarative_environment: u32,
    /// The pointer to the first `GlobalVariableCache` of an `Executable`.
    pub executable_global_variable_caches: u32,
    /// `sizeof(GlobalVariableCache)`.
    pub global_variable_cache_size: u32,
    /// `GlobalVariableCache::environment_serial_number` (u64).
    pub global_variable_cache_environment_serial: u32,
    /// `GlobalVariableCache::environment_binding_index` (u32).
    pub global_variable_cache_environment_binding_index: u32,
    /// `GlobalVariableCache::has_environment_binding_index` (u8).
    pub global_variable_cache_has_environment_binding: u32,
    /// The shape (cell pointer) of the cache entry of a `GlobalVariableCache`.
    pub global_variable_cache_shape: u32,
    /// The shape dictionary generation (u32) of the cache entry.
    pub global_variable_cache_dictionary_generation: u32,
    /// The property offset (u32) of the cache entry.
    pub global_variable_cache_property_offset: u32,
    /// Whether the cache entry writes a data property (bool).
    pub global_variable_cache_writes_data_property: u32,
    /// The flag bit of a mutable binding.
    pub binding_flag_mutable: u8,
    /// The flag bit of a strict binding.
    pub binding_flag_strict: u8,
    /// The flag bit of a binding that can be deleted.
    pub binding_flag_can_be_deleted: u8,
    /// `ObjectPropertyIteratorCacheData::fast_path` (u8).
    pub property_iterator_fast_path: u32,
    /// `ObjectPropertyIteratorCacheData::shape` (cell pointer).
    pub property_iterator_shape: u32,
    /// `ObjectPropertyIteratorCacheData::shape_is_dictionary` (bool).
    pub property_iterator_shape_is_dictionary: u32,
    /// `ObjectPropertyIteratorCacheData::shape_dictionary_generation` (u32).
    pub property_iterator_shape_dictionary_generation: u32,
    /// `ObjectPropertyIteratorCacheData::indexed_property_count` (u32).
    pub property_iterator_indexed_property_count: u32,
    /// `ObjectPropertyIteratorCacheData::prototype_chain_validity` (cell pointer, may be null).
    pub property_iterator_prototype_chain_validity: u32,
    /// `ObjectPropertyIteratorCacheData::property_values.data`, the pointer to the first `Value`.
    pub property_iterator_property_values: u32,
    /// The size (u64) of `ObjectPropertyIteratorCacheData::property_values`.
    pub property_iterator_property_value_count: u32,
    /// `ObjectPropertyIteratorFastPath::None`.
    pub property_iterator_fast_path_none: u8,
    /// `ObjectPropertyIteratorFastPath::PackedIndexed`.
    pub property_iterator_fast_path_packed_indexed: u8,
    /// The pointer to the first `PropertyLookupCache` of an `Executable`.
    pub executable_property_lookup_caches: u32,
    /// Masks the tag bits off `PropertyLookupCache::data`, leaving the pointer to its most recently used entry.
    pub property_lookup_cache_data_pointer_mask: u64,
    /// `PropertyLookupCache::Entry::type` (u32).
    pub property_lookup_cache_entry_type: u32,
    /// `PropertyLookupCache::Entry::property_offset` (u32).
    pub property_lookup_cache_entry_property_offset: u32,
    /// `PropertyLookupCache::Entry::shape_dictionary_generation` (u32).
    pub property_lookup_cache_entry_dictionary_generation: u32,
    /// `PropertyLookupCache::Entry::writes_data_property` (bool).
    pub property_lookup_cache_entry_writes_data_property: u32,
    /// `PropertyLookupCache::Entry::shape` (cell pointer).
    pub property_lookup_cache_entry_shape: u32,
    /// `PropertyLookupCache::Entry::prototype` (cell pointer, may be null).
    pub property_lookup_cache_entry_prototype: u32,
    /// `PropertyLookupCache::Entry::prototype_chain_validity` (cell pointer, may be null).
    pub property_lookup_cache_entry_prototype_chain_validity: u32,
    /// `PropertyLookupCache::Entry::Type::GetMissingProperty`.
    pub property_lookup_cache_entry_type_get_missing_property: u32,
    /// `PropertyLookupCache::Entry::Type::AddOwnProperty`.
    pub property_lookup_cache_entry_type_add_own_property: u32,
    /// `PropertyLookupCache::Entry::Type::GetOwnProperty`.
    pub property_lookup_cache_entry_type_get_own_property: u32,
    /// `PropertyLookupCache::Entry::Type::ChangeOwnProperty`.
    pub property_lookup_cache_entry_type_change_own_property: u32,
    /// `Class::object_methods`, of the class every cell starts with, or 0 if
    /// JIT code does not walk prototype chains.
    pub class_object_methods: u32,
    /// `ObjectMethods::internal_get_prototype_of`, which is null for objects
    /// whose prototype is the prototype of their shape.
    pub object_methods_get_prototype_of: u32,
    /// The entries of the VM's keyed property lookup cache, which remembers
    /// lookups of names on shapes for keyed accesses whose own caches do not
    /// have them, or 0 if JIT code does not look in it. The entry for a name
    /// whose identity (the storage word of its fly string) is `name` on a
    /// shape is the one at the top `keyed_lookup_cache_index_bits` bits of
    /// `(shape as u32 ^ name as u32) * property_lookup_cache_megamorphic_hash_multiplier`.
    pub keyed_lookup_cache_entries: u64,
    /// The entries of the VM's keyed property store cache, laid out and
    /// found like those of its keyed property lookup cache, whose
    /// `ChangeOwnProperty` entries remember the writable own data properties
    /// that string-keyed stores change, or 0 if JIT code does not look in it.
    pub keyed_store_cache_entries: u64,
    pub keyed_lookup_cache_index_bits: u32,
    /// The size of an entry, a power of two.
    pub keyed_lookup_cache_entry_size: u32,
    /// The offsets in an entry of its type (u32, a
    /// `PropertyLookupCache::Entry::Type`), property offset (u32), dictionary
    /// generation (u32), shape (cell pointer) and name identity (word).
    pub keyed_lookup_cache_entry_type: u32,
    pub keyed_lookup_cache_property_offset: u32,
    pub keyed_lookup_cache_dictionary_generation: u32,
    pub keyed_lookup_cache_shape: u32,
    pub keyed_lookup_cache_name: u32,
    /// The tag bits of `PropertyLookupCache::data` of a polymorphic cache,
    /// whose entries follow its most recently used one.
    pub property_lookup_cache_polymorphic_tag: u64,
    /// How many entries a polymorphic cache has.
    pub property_lookup_cache_polymorphic_entry_count: u32,
    /// `sizeof(PropertyLookupCache::Entry)`.
    pub property_lookup_cache_entry_size: u32,
    /// `PropertyLookupCache::Entry::key` (u64): the encoded key Value of a keyed access's entry.
    pub property_lookup_cache_entry_key: u32,
    /// `PropertyLookupCache::data` of a keyed access that saw too many keys to cache.
    pub property_lookup_cache_keyed_generic: u64,
    /// `PropertyLookupCache::Entry::from_shape` (cell pointer): the shape an
    /// `AddOwnProperty` entry transitions from.
    pub property_lookup_cache_entry_from_shape: u32,
    /// The tag of `PropertyLookupCache::data` for `MegamorphicData`.
    pub property_lookup_cache_megamorphic_tag: u64,
    /// `PropertyLookupCache::MegamorphicData::primary_entries` (entries).
    pub property_lookup_cache_megamorphic_primary_entries: u32,
    /// `PropertyLookupCache::MegamorphicData::secondary_entries` (entries).
    pub property_lookup_cache_megamorphic_secondary_entries: u32,
    /// `PropertyLookupCache::megamorphic_index_bits`: the size of both
    /// megamorphic tables is two to this power.
    pub property_lookup_cache_megamorphic_index_bits: u32,
    /// `PropertyLookupCache::megamorphic_hash_multiplier`: the hash of an
    /// entry is the low 32 bits of `shape ^ key` times this (in 32 bits),
    /// its primary index the top `index_bits` bits of that and its
    /// secondary index the next ones.
    pub property_lookup_cache_megamorphic_hash_multiplier: u32,
    /// `u64 asm_try_get_by_id_cache(u64 base, PropertyLookupCache*)`: the value of the
    /// property from any entry of the cache, or the empty value.
    pub try_get_by_id_cache: u64,
    /// `i64 asm_try_put_by_id_cache(VM*, u32 pc, Op::PutById const*, Op::PutById::Values&)`:
    /// stores through any entry of the running executable's cache, returning 0 if it did.
    pub try_put_by_id_cache: u64,
}
