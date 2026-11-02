/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, OnceCell, RefCell};
use std::collections::HashSet;
use std::rc::Rc;

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

pub use crate::ast::FunctionKind;
use crate::ast::FunctionPayload;
use crate::bytecode::bytecode_cache::materialize_cached_function_executable;
use crate::bytecode::executable::Executable;
use crate::bytecode::generator::{FunctionSfdMetadata, PendingSharedFunctionData, PrecompiledFunction};
use crate::bytecode_cache::{DecodedCachedExecutableRecord, DecodedFunctionRecord};
use crate::compile::FunctionPrecompileMode;
use crate::compile::SharedFunctionDescription;
use crate::gc::class::{GcCell, define_cell};
use crate::gc::defer_gc::DeferGc;
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::run::should_dump_bytecode;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeaderTail, Gc};
use crate::layout::execution_context::{ScriptOrModule, ScriptOrModuleSlot};
use crate::layout::function_object::CallEnvironmentTemplate;
pub use crate::layout::function_object::{SharedFunctionInstanceData, asm_call_metadata};
use crate::layout::member::NullableMember;
use crate::runtime::environment_shape::{EnvironmentShape, EnvironmentShapeCache};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::shape::Shape;
use crate::source_code::SourceCode;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ThisMode {
    Lexical,
    Strict,
    Global,
}

/// How many closures of a function were created.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ClosureCount {
    None,
    One,
    Many,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ConstructorKind {
    Base,
    Derived,
}

/// [[ClassFieldInitializerName]]: Variant<PropertyKey, PrivateName, Empty>. Only whether there is a name matters, so a
/// Private Name is kept as its description.
#[derive(Clone, Debug, Default, Trace)]
pub enum ClassFieldInitializerName {
    PropertyKey(PropertyKey),
    PrivateName(#[gc(untraced)] Utf16FlyString),
    #[default]
    Empty,
}

/// What a function's executable is made from when it is first called, which only one thing can be at a time.
enum CompileInput {
    None,
    /// The function's AST, which its bytecode is compiled from.
    FunctionAst(Box<FunctionPayload>),
    /// Freshly compiled bytecode.
    PrecompiledBytecode(Box<PrecompiledFunction>),
    /// The function's executable in a bytecode cache blob.
    CachedBytecode(Box<DecodedCachedExecutableRecord>),
}

/// A CompileInput in one word: the pointer, with what it points at in its lowest bits.
struct CompileInputSlot(Cell<*mut ()>);

// NB: The pointers in a slot leave room for its tag.
const _: () = assert!(
    align_of::<FunctionPayload>() > CompileInputSlot::TAG_MASK
        && align_of::<PrecompiledFunction>() > CompileInputSlot::TAG_MASK
        && align_of::<DecodedCachedExecutableRecord>() > CompileInputSlot::TAG_MASK
);

impl CompileInputSlot {
    const TAG_MASK: usize = 0b11;
    const TAG_FUNCTION_AST: usize = 1;
    const TAG_PRECOMPILED_BYTECODE: usize = 2;
    const TAG_CACHED_BYTECODE: usize = 3;

    fn new(input: CompileInput) -> Self {
        let slot = Self(Cell::new(core::ptr::null_mut()));
        slot.set(input);
        slot
    }

    fn set(&self, input: CompileInput) {
        drop(self.take());
        self.0.set(match input {
            CompileInput::None => core::ptr::null_mut(),
            CompileInput::FunctionAst(ast) => Self::tagged(Box::into_raw(ast), Self::TAG_FUNCTION_AST),
            CompileInput::PrecompiledBytecode(precompiled) => {
                Self::tagged(Box::into_raw(precompiled), Self::TAG_PRECOMPILED_BYTECODE)
            }
            CompileInput::CachedBytecode(cached) => Self::tagged(Box::into_raw(cached), Self::TAG_CACHED_BYTECODE),
        });
    }

    fn tagged<T>(pointer: *mut T, tag: usize) -> *mut () {
        pointer.cast::<()>().map_addr(|address| address | tag)
    }

    fn take(&self) -> CompileInput {
        let word = self.0.replace(core::ptr::null_mut());
        let pointer = word.map_addr(|address| address & !Self::TAG_MASK);
        // SAFETY: set() made the word from a box of the type its tag says, which nothing else owns now that the word
        //         is gone from the slot.
        unsafe {
            match word.addr() & Self::TAG_MASK {
                Self::TAG_FUNCTION_AST => CompileInput::FunctionAst(Box::from_raw(pointer.cast())),
                Self::TAG_PRECOMPILED_BYTECODE => CompileInput::PrecompiledBytecode(Box::from_raw(pointer.cast())),
                Self::TAG_CACHED_BYTECODE => CompileInput::CachedBytecode(Box::from_raw(pointer.cast())),
                _ => CompileInput::None,
            }
        }
    }

    fn tag(&self) -> usize {
        self.0.get().addr() & Self::TAG_MASK
    }
}

impl Drop for CompileInputSlot {
    fn drop(&mut self) {
        drop(self.take());
    }
}

/// The names of the parameters of a function with a simple parameter list that has any.
struct ParameterNames {
    /// The name each argument index of a mapped arguments object maps to, which is empty where a later parameter has
    /// the same name.
    mapped_argument_names: Rc<[Utf16FlyString]>,
    /// The names of the parameters without the ones that repeat, in the order the bindings of the parameters are
    /// created in the function environment.
    parameter_binding_names: Rc<[Utf16FlyString]>,
}

/// The parts of the shared data of a function that few functions need.
#[derive(Default, Trace)]
struct SharedFunctionInstanceDataRareData {
    /// Owns the source text if it is not simply a range of the source code the function was parsed from, as for
    /// dynamically created functions via the Function constructor, or once something asked for it.
    #[gc(untraced)]
    source_text_owner: RefCell<Utf16String>,
    class_field_initializer_name: GcRefCell<ClassFieldInitializerName>, // [[ClassFieldInitializerName]]
    /// The source text range the function was created with, which a bytecode cache blob identifies the function by,
    /// once the source text range of the function no longer is that, as when a class constructor's grows to the whole
    /// class.
    #[gc(untraced)]
    bytecode_cache_source_text_range: Cell<Option<(u32, u32)>>,
    /// Functions with parameter expressions may have a separate var environment.
    var_environment_bindings_count: Cell<u32>,
    var_environment_shape: NullableMember<EnvironmentShape>,
}

/// The parts of the shared data of a function that the interpreter does not read.
#[derive(Trace)]
pub struct SharedFunctionInstanceDataStorage {
    name: Utf16FlyString,

    // NB: source_text_offset and source_text_length are a range of the source code we parsed the AST from.
    #[gc(untraced)]
    source_code: Cell<Option<Rc<SourceCode>>>,
    source_text_offset: Cell<u32>,
    source_text_length: Cell<u32>, // [[SourceText]]

    #[gc(untraced)]
    parameter_names: Option<Box<ParameterNames>>,
    #[gc(untraced)]
    compile_input: CompileInputSlot,
    rare_data: OnceCell<Box<SharedFunctionInstanceDataRareData>>,

    function_environment_bindings_count: Cell<u32>,
    function_environment_shape: NullableMember<EnvironmentShape>,
}

// NB: Every function has these bytes.
const _: () = assert!(size_of::<SharedFunctionInstanceData>() == 88);

/// The most properties that constructions make room for ahead (the largest inline capacity of plain objects).
const MAX_CONSTRUCT_RESERVE: u32 = 16;

/// The last construction that follows the transitions again (see SharedFunctionInstanceData::construct_reserve()).
const MAX_CONSTRUCT_RESERVE_UPDATES: u32 = 1024;

const _: () = assert!(MAX_CONSTRUCT_RESERVE <= u8::MAX as u32);
const _: () = assert!(MAX_CONSTRUCT_RESERVE_UPDATES as u64 <= asm_call_metadata::CONSTRUCT_COUNT_MASK);

define_cell!(SharedFunctionInstanceData, Other);

// SAFETY: Visits the executable, the call environment template and every cell the storage holds.
unsafe impl Trace for SharedFunctionInstanceData {
    fn trace(&self, visitor: &mut Visitor) {
        self.executable.trace(visitor);
        self.call_environment_template.trace(visitor);
        match self.script_or_module.get() {
            ScriptOrModule::Empty => {}
            ScriptOrModule::Script(script) => script.trace(visitor),
            ScriptOrModule::Module(module) => module.trace(visitor),
        }
        self.storage.trace(visitor);
    }
}

define_cell!(CallEnvironmentTemplate, Other);

// SAFETY: The words of a template refer to the environment's shape, which the shared function instance data keeps
//         alive, and to no other cell.
unsafe impl Trace for CallEnvironmentTemplate {
    fn trace(&self, _visitor: &mut Visitor) {}
}

/// OPTIMIZATION: Precompute the argument indices that alias parameter bindings. Only the last occurrence of each
///               parameter name is eligible; each arguments object also limits the mapping to its argument count.
///               https://tc39.es/ecma262/#sec-createmappedargumentsobject
fn parameter_names_of(parameter_names: Vec<Utf16FlyString>) -> Option<Box<ParameterNames>> {
    if parameter_names.is_empty() {
        return None;
    }
    let mut seen_names = HashSet::with_capacity(parameter_names.len());
    let mut mapped_names: Vec<Utf16FlyString> = vec![Utf16FlyString::default(); parameter_names.len()];
    let mut has_repeated_name = false;

    for index in (0..parameter_names.len()).rev() {
        let name = &parameter_names[index];
        // OPTIMIZATION: A hash set avoids scanning later parameters for every name.
        if !seen_names.insert(name) {
            has_repeated_name = true;
            continue;
        }
        mapped_names[index] = name.clone();
    }
    let mapped_argument_names: Rc<[Utf16FlyString]> = mapped_names.into();

    // NB: Without repeated names, both lists are the parameter names.
    let parameter_binding_names = if has_repeated_name {
        let mut seen_names = HashSet::with_capacity(parameter_names.len());
        let mut names: Vec<Utf16FlyString> = Vec::with_capacity(parameter_names.len());
        for name in &parameter_names {
            if seen_names.insert(name) {
                names.push(name.clone());
            }
        }
        names.into()
    } else {
        mapped_argument_names.clone()
    };

    Some(Box::new(ParameterNames {
        mapped_argument_names,
        parameter_binding_names,
    }))
}

fn source_text_offset_or_length(offset_or_length: usize) -> u32 {
    u32::try_from(offset_or_length).expect("a source code fits u32 code units")
}

impl SharedFunctionInstanceData {
    #[allow(clippy::too_many_arguments)]
    fn new(
        _: &DeferGc<'_>,
        kind: FunctionKind,
        name: Utf16FlyString,
        function_length: i32,
        formal_parameter_count: u32,
        strict: bool,
        is_arrow_function: bool,
        has_simple_parameter_list: bool,
        parameter_names_for_mapped_arguments: Vec<Utf16FlyString>,
        function_ast: Option<Box<FunctionPayload>>,
    ) -> Self {
        // NB: initialize_after_construction()
        let mut metadata = u64::from(formal_parameter_count)
            | asm_call_metadata::MIGHT_NEED_ARGUMENTS_OBJECT
            | asm_call_metadata::CONTAINS_DIRECT_CALL_TO_EVAL
            | (kind as u64) << asm_call_metadata::FUNCTION_KIND_SHIFT;
        if strict {
            metadata |= asm_call_metadata::STRICT;
        }
        if is_arrow_function {
            metadata |= asm_call_metadata::IS_ARROW_FUNCTION;
        }
        if has_simple_parameter_list {
            metadata |= asm_call_metadata::HAS_SIMPLE_PARAMETER_LIST;
        }

        Self {
            executable: NullableMember::null(),
            call_environment_template: NullableMember::null(),
            header: CellHeaderTail::for_class(Self::CLASS),
            construct_reserve: Cell::new(0),
            function_length,
            asm_call_metadata: Cell::new(metadata),
            script_or_module: ScriptOrModuleSlot::new(ScriptOrModule::Empty),
            storage: SharedFunctionInstanceDataStorage {
                name,
                source_code: Cell::new(None),
                source_text_offset: Cell::new(0),
                source_text_length: Cell::new(0),
                parameter_names: parameter_names_of(parameter_names_for_mapped_arguments),
                compile_input: CompileInputSlot::new(
                    function_ast.map_or(CompileInput::None, CompileInput::FunctionAst),
                ),
                rare_data: OnceCell::new(),
                function_environment_bindings_count: Cell::new(0),
                function_environment_shape: NullableMember::null(),
            },
        }
    }

    fn rare_data(&self) -> &SharedFunctionInstanceDataRareData {
        self.storage.rare_data.get_or_init(Box::default)
    }

    fn metadata_bit(&self, bit: u64) -> bool {
        self.asm_call_metadata.get() & bit != 0
    }

    fn set_metadata_bit(&self, bit: u64, value: bool) {
        let mut metadata = self.asm_call_metadata.get() & !bit;
        if value {
            metadata |= bit;
        }
        metadata &= !asm_call_metadata::NEEDS_ENVIRONMENT_OR_THIS_VALUE_RESOLUTION;
        if metadata
            & (asm_call_metadata::FUNCTION_ENVIRONMENT_NEEDED
                | asm_call_metadata::THIS_VALUE_NEEDS_ENVIRONMENT_RESOLUTION)
            != 0
        {
            metadata |= asm_call_metadata::NEEDS_ENVIRONMENT_OR_THIS_VALUE_RESOLUTION;
        }
        self.asm_call_metadata.set(metadata);
    }

    /// Creates the shared data of a function the frontend described.
    pub fn create(
        vm: &Vm,
        description: SharedFunctionDescription,
        source_code: Option<&Rc<SourceCode>>,
    ) -> Gc<SharedFunctionInstanceData> {
        let name = if description.name.is_empty() {
            Utf16FlyString::default()
        } else {
            Utf16FlyString::from_utf16(&description.name)
        };

        let mapped_parameter_names = if description.has_simple_parameter_list {
            description.parameter_names
        } else {
            Vec::new()
        };

        let shared = vm.heap().allocate(|defer| {
            Self::new(
                defer,
                description.function_kind,
                name,
                description.function_length,
                description.formal_parameter_count,
                description.strict,
                description.is_arrow,
                description.has_simple_parameter_list,
                mapped_parameter_names,
                Some(description.payload),
            )
        });
        shared.update_can_inline_call();

        // Set parsing insights that must be available before lazy compilation.
        shared.set_uses_this(description.uses_this);
        shared.set_metadata_bit(
            asm_call_metadata::THIS_VALUE_NEEDS_ENVIRONMENT_RESOLUTION,
            description.uses_this_from_environment,
        );
        if description.uses_this_from_environment && !description.is_arrow {
            shared.set_function_environment_needed(true);
        }

        shared.storage.source_code.set(source_code.cloned());
        shared.initialize_source_text_range(description.source_text_offset, description.source_text_length);
        shared
    }

    /// Creates the shared data of a function of a bytecode cache blob that passed validation, whose executable stays in
    /// the blob until the function is first called. The function is strict if its own code is or `outer_strict` is.
    pub fn create_from_bytecode_cache(
        vm: &Vm,
        function: &DecodedFunctionRecord,
        outer_strict: bool,
        source_code: &Rc<SourceCode>,
    ) -> Gc<SharedFunctionInstanceData> {
        let name = function
            .function_name()
            .map_or_else(Utf16FlyString::default, |name| Utf16FlyString::from_utf16(&name));
        let simple_parameter_names = function.simple_parameter_names();
        let has_simple_parameter_list = simple_parameter_names.is_some();
        let source_text_range = function.source_text_range();
        let source_text_length = source_text_range.end.saturating_sub(source_text_range.start);
        let is_arrow_function = function.is_arrow();

        let shared = vm.heap().allocate(|defer| {
            Self::new(
                defer,
                function.function_kind(),
                name,
                function.length(),
                function.parameter_count(),
                function.has_strict_code() || outer_strict,
                is_arrow_function,
                has_simple_parameter_list,
                simple_parameter_names.unwrap_or_default(),
                None,
            )
        });
        shared.update_can_inline_call();

        // Set parsing insights that must be available before lazy compilation.
        let (uses_this, uses_this_from_environment) = function.parsing_insights_about_this();
        shared.set_uses_this(uses_this);
        shared.set_metadata_bit(
            asm_call_metadata::THIS_VALUE_NEEDS_ENVIRONMENT_RESOLUTION,
            uses_this_from_environment,
        );
        if uses_this_from_environment && !is_arrow_function {
            shared.set_function_environment_needed(true);
        }

        shared.storage.source_code.set(Some(source_code.clone()));
        shared.initialize_source_text_range(source_text_range.start, source_text_length);
        if let Some((name, is_private)) = function.field_initializer_name() {
            shared.set_class_field_initializer_name_from_utf16(&name, is_private);
        }
        shared.set_metadata(function.scope_metadata());
        shared
            .storage
            .compile_input
            .set(CompileInput::CachedBytecode(Box::new(function.cached_executable())));
        shared
    }

    fn initialize_source_text_range(&self, source_text_offset: usize, source_text_length: usize) {
        self.storage
            .source_text_offset
            .set(source_text_offset_or_length(source_text_offset));
        self.storage
            .source_text_length
            .set(source_text_offset_or_length(source_text_length));
    }

    /// The offset and length of the source text the function was created with.
    pub fn bytecode_cache_source_text_range(&self) -> (usize, usize) {
        let (offset, length) = self
            .storage
            .rare_data
            .get()
            .and_then(|rare_data| rare_data.bytecode_cache_source_text_range.get())
            .unwrap_or((
                self.storage.source_text_offset.get(),
                self.storage.source_text_length.get(),
            ));
        (offset as usize, length as usize)
    }

    /// Keeps the range a bytecode cache blob identifies the function by, as the source text range is about to change.
    fn preserve_bytecode_cache_source_text_range(&self) {
        let range = (
            self.storage.source_text_offset.get(),
            self.storage.source_text_length.get(),
        );
        let slot = &self.rare_data().bytecode_cache_source_text_range;
        if slot.get().is_none() {
            slot.set(Some(range));
        }
    }

    /// Whether `function`, a function of a bytecode cache blob nested in code that is strict if `outer_strict` is, is
    /// the function this shared data was created for, as rust_sfd_matches_bytecode_cache_function decides.
    pub fn matches_bytecode_cache_function(&self, function: &DecodedFunctionRecord, outer_strict: bool) -> bool {
        let source_text_range = function.source_text_range();
        self.bytecode_cache_source_text_range()
            == (
                source_text_range.start,
                source_text_range.end.saturating_sub(source_text_range.start),
            )
            && self.function_length == function.length()
            && self.formal_parameter_count() == function.parameter_count()
            && self.kind() == function.function_kind()
            && self.strict() == (function.has_strict_code() || outer_strict)
            && self.is_arrow_function() == function.is_arrow()
            && self.has_simple_parameter_list() == function.simple_parameter_names().is_some()
    }

    /// Replaces how the function compiles with its executable in a bytecode cache blob, which becomes its executable
    /// when it is first called, as rust_sfd_install_cached_bytecode_executable does.
    pub fn install_cached_bytecode_executable(
        &self,
        cached_executable: DecodedCachedExecutableRecord,
        metadata: &FunctionSfdMetadata,
    ) {
        self.set_metadata(metadata);
        self.storage
            .compile_input
            .set(CompileInput::CachedBytecode(Box::new(cached_executable)));
    }

    /// Replaces the executable of a function that already ran with one materialized from a bytecode cache blob, which
    /// took over the inline caches of the previous one, as rust_sfd_install_bytecode_cache_executable does.
    pub fn install_bytecode_cache_executable(&self, executable: Gc<Executable>, metadata: &FunctionSfdMetadata) {
        self.set_metadata(metadata);
        self.set_executable(Some(executable));
        executable.set_name(self.name());
        if should_dump_bytecode() {
            executable.dump();
        }
        self.clear_compile_inputs();
    }

    /// vm.heap().allocate<SharedFunctionInstanceData>(vm, kind, name, function_length, formal_parameter_count, strict,
    /// is_arrow_function, has_simple_parameter_list, parameter_names_for_mapped_arguments, NoSharedFunctionDataList {},
    /// nullptr): shared data that has no AST to compile, such as the wrapper of a module with top-level await, whose
    /// executable the caller sets.
    #[allow(clippy::too_many_arguments)]
    pub fn create_without_function_ast(
        vm: &Vm,
        kind: FunctionKind,
        name: Utf16FlyString,
        function_length: i32,
        formal_parameter_count: u32,
        strict: bool,
        is_arrow_function: bool,
        has_simple_parameter_list: bool,
        parameter_names_for_mapped_arguments: Vec<Utf16FlyString>,
    ) -> Gc<SharedFunctionInstanceData> {
        let shared = vm.heap().allocate(|defer| {
            Self::new(
                defer,
                kind,
                name,
                function_length,
                formal_parameter_count,
                strict,
                is_arrow_function,
                has_simple_parameter_list,
                parameter_names_for_mapped_arguments,
                None,
            )
        });
        shared.update_can_inline_call();
        shared
    }

    /// Creates the shared data of one of the functions an executable declares, taking what the frontend kept for it:
    /// its AST, its class field initializer name and the bytecode it may have compiled already.
    pub fn create_from_pending_shared_function_data(
        vm: &Vm,
        pending: &mut PendingSharedFunctionData,
        is_strict: bool,
        source_code: Option<&Rc<SourceCode>>,
    ) -> Gc<SharedFunctionInstanceData> {
        let shared = Self::create(vm, pending.take_description(is_strict), source_code);
        if let Some((name, is_private)) = &pending.class_field_initializer_name {
            shared.set_class_field_initializer_name_from_utf16(name, *is_private);
        }
        if let Some(precompiled) = pending.precompiled_function.take() {
            shared.set_precompiled_bytecode_executable(precompiled);
        }
        shared
    }

    pub fn executable(&self) -> Option<Gc<Executable>> {
        self.executable.get().map(Executable::from_head)
    }

    pub fn set_executable(&self, executable: Option<Gc<Executable>>) {
        self.executable.set(executable.map(Executable::head));
        self.update_can_inline_call();
    }

    pub fn set_is_class_constructor(&self) {
        self.set_metadata_bit(asm_call_metadata::IS_CLASS_CONSTRUCTOR, true);
        self.update_can_inline_call();
    }

    pub fn can_inline_call(&self) -> bool {
        self.metadata_bit(asm_call_metadata::CAN_INLINE_CALL)
    }

    fn update_can_inline_call(&self) {
        self.set_metadata_bit(
            asm_call_metadata::CAN_INLINE_CALL,
            self.executable.get().is_some() && self.kind() == FunctionKind::Normal && !self.is_class_constructor(),
        );
    }

    pub fn name(&self) -> Utf16FlyString {
        self.storage.name.clone()
    }

    pub fn source_text_owner(&self) -> Option<Utf16String> {
        let source_text_owner = self.storage.rare_data.get()?.source_text_owner.borrow();
        (!source_text_owner.is_empty()).then(|| source_text_owner.clone())
    }

    pub fn source_text(&self) -> Utf16String {
        if let Some(source_text_owner) = self.source_text_owner() {
            return source_text_owner;
        }

        let Some(source_code) = self.source_code() else {
            return Utf16String::default();
        };

        let source_text = source_code.source_text_from_offsets(
            self.storage.source_text_offset.get() as usize,
            self.storage.source_text_length.get() as usize,
        );
        self.rare_data().source_text_owner.replace(source_text.clone());
        source_text
    }

    /// Makes `source_text` the [[SourceText]] while keeping the source code the function compiles from.
    pub fn set_source_text_owner(&self, source_text: Utf16String) {
        self.rare_data().source_text_owner.replace(source_text);
    }

    pub fn set_source_text(&self, source_text: Utf16String) {
        self.preserve_bytecode_cache_source_text_range();
        self.rare_data().source_text_owner.replace(source_text);
        self.storage.source_code.set(None);
        self.storage.source_text_offset.set(0);
        self.storage.source_text_length.set(0);
    }

    pub fn set_source_text_range(
        &self,
        source_code: &Rc<SourceCode>,
        source_text_offset: usize,
        source_text_length: usize,
    ) {
        self.preserve_bytecode_cache_source_text_range();
        self.rare_data().source_text_owner.replace(Utf16String::default());
        self.storage.source_code.set(Some(source_code.clone()));
        self.initialize_source_text_range(source_text_offset, source_text_length);
    }

    pub fn source_code(&self) -> Option<Rc<SourceCode>> {
        let source_code = self.storage.source_code.take();
        self.storage.source_code.set(source_code.clone());
        source_code
    }

    pub fn clear_compile_inputs(&self) {
        self.storage.compile_input.set(CompileInput::None);
    }

    pub fn clear_non_bytecode_cache_compile_inputs(&self) {
        if self.storage.compile_input.tag() != CompileInputSlot::TAG_CACHED_BYTECODE {
            self.clear_compile_inputs();
        }
    }

    /// Whether the function still has the AST it compiles from when it is first called.
    pub fn has_function_ast(&self) -> bool {
        self.storage.compile_input.tag() == CompileInputSlot::TAG_FUNCTION_AST
    }

    /// Whether the function still has bytecode compiled ahead of its first call, apart from that of a bytecode cache.
    pub fn has_precompiled_bytecode(&self) -> bool {
        self.storage.compile_input.tag() == CompileInputSlot::TAG_PRECOMPILED_BYTECODE
    }

    /// Whether the function's executable is still in a bytecode cache blob, waiting for its first call.
    pub fn has_cached_bytecode(&self) -> bool {
        self.storage.compile_input.tag() == CompileInputSlot::TAG_CACHED_BYTECODE
    }

    /// Appends the functions that `executable` creates and that have not been compiled yet to `functions`, and returns
    /// a copy of the AST of each, so that they can be compiled elsewhere, such as on another thread, while their own
    /// ASTs stay in place for a first call that comes sooner.
    pub fn uncompiled_functions_of(
        executable: Gc<Executable>,
        functions: &MarkedVec<'_, Gc<SharedFunctionInstanceData>>,
    ) -> Vec<Box<FunctionPayload>> {
        let mut function_asts = Vec::new();
        for index in 0..executable.shared_function_data_count() {
            let shared_data = executable.shared_function_data(u32::try_from(index).expect("the index fits in u32"));
            if shared_data.executable().is_some() {
                continue;
            }
            if !shared_data.has_function_ast() {
                continue;
            }
            let CompileInput::FunctionAst(function_ast) = shared_data.storage.compile_input.take() else {
                unreachable!("the function has its AST");
            };
            functions.push(shared_data);
            function_asts.push(function_ast.clone());
            shared_data
                .storage
                .compile_input
                .set(CompileInput::FunctionAst(function_ast));
        }
        function_asts
    }

    /// Records what scope analysis found out about the function's body, as rust_sfd_set_metadata does.
    pub fn set_metadata(&self, metadata: &FunctionSfdMetadata) {
        self.set_uses_this(metadata.uses_this);
        self.set_metadata_bit(
            asm_call_metadata::THIS_VALUE_NEEDS_ENVIRONMENT_RESOLUTION,
            metadata.this_value_needs_environment_resolution,
        );
        self.set_function_environment_needed(metadata.function_environment_needed);
        self.storage
            .function_environment_bindings_count
            .set(u32::try_from(metadata.function_environment_bindings_count).expect("the count fits in u32"));
        if metadata.var_environment_bindings_count != 0 || self.storage.rare_data.get().is_some() {
            self.rare_data()
                .var_environment_bindings_count
                .set(u32::try_from(metadata.var_environment_bindings_count).expect("the count fits in u32"));
        }
        self.set_metadata_bit(
            asm_call_metadata::MIGHT_NEED_ARGUMENTS_OBJECT,
            metadata.might_need_arguments,
        );
        self.set_metadata_bit(asm_call_metadata::CONTAINS_DIRECT_CALL_TO_EVAL, metadata.contains_eval);
    }

    /// Keeps bytecode the frontend compiled ahead of the first call, as rust_sfd_set_precompiled_bytecode_executable
    /// does.
    pub fn set_precompiled_bytecode_executable(&self, precompiled: Box<PrecompiledFunction>) {
        self.set_metadata(&precompiled.metadata);
        self.storage
            .compile_input
            .set(CompileInput::PrecompiledBytecode(precompiled));
    }

    /// Sets [[ClassFieldInitializerName]] to a name the frontend found, as rust_sfd_set_class_field_initializer_name
    /// does.
    pub fn set_class_field_initializer_name_from_utf16(&self, name: &[u16], is_private: bool) {
        let name = Utf16FlyString::from_utf16(name);
        self.set_class_field_initializer_name(if is_private {
            ClassFieldInitializerName::PrivateName(name)
        } else {
            ClassFieldInitializerName::PropertyKey(PropertyKey::from(name))
        });
    }

    pub fn class_field_initializer_name(&self) -> ClassFieldInitializerName {
        self.storage
            .rare_data
            .get()
            .map_or(ClassFieldInitializerName::Empty, |rare_data| {
                rare_data.class_field_initializer_name.borrow().clone()
            })
    }

    pub fn set_class_field_initializer_name(&self, name: ClassFieldInitializerName) {
        self.rare_data().class_field_initializer_name.replace(name);
    }

    pub fn kind(&self) -> FunctionKind {
        let kind = self.asm_call_metadata.get() >> asm_call_metadata::FUNCTION_KIND_SHIFT
            & asm_call_metadata::FUNCTION_KIND_MASK;
        match kind {
            0 => FunctionKind::Normal,
            1 => FunctionKind::Generator,
            2 => FunctionKind::Async,
            _ => FunctionKind::AsyncGenerator,
        }
    }

    pub fn this_mode(&self) -> ThisMode {
        if self.is_arrow_function() {
            ThisMode::Lexical
        } else if self.strict() {
            ThisMode::Strict
        } else {
            ThisMode::Global
        }
    }

    pub fn function_length(&self) -> i32 {
        self.function_length
    }

    pub fn formal_parameter_count(&self) -> u32 {
        self.asm_call_metadata.get() as u32
    }

    pub fn mapped_argument_names(&self) -> Rc<[Utf16FlyString]> {
        self.storage.parameter_names.as_ref().map_or_else(
            || Vm::of_cell(self).no_parameter_names.clone(),
            |names| names.mapped_argument_names.clone(),
        )
    }

    pub fn parameter_binding_names(&self) -> Rc<[Utf16FlyString]> {
        self.storage.parameter_names.as_ref().map_or_else(
            || Vm::of_cell(self).no_parameter_names.clone(),
            |names| names.parameter_binding_names.clone(),
        )
    }

    pub fn strict(&self) -> bool {
        self.metadata_bit(asm_call_metadata::STRICT)
    }

    pub fn is_arrow_function(&self) -> bool {
        self.metadata_bit(asm_call_metadata::IS_ARROW_FUNCTION)
    }

    pub fn has_simple_parameter_list(&self) -> bool {
        self.metadata_bit(asm_call_metadata::HAS_SIMPLE_PARAMETER_LIST)
    }

    pub fn is_module_wrapper(&self) -> bool {
        self.metadata_bit(asm_call_metadata::IS_MODULE_WRAPPER)
    }

    pub fn set_is_module_wrapper(&self, is_module_wrapper: bool) {
        self.set_metadata_bit(asm_call_metadata::IS_MODULE_WRAPPER, is_module_wrapper);
    }

    pub fn is_class_constructor(&self) -> bool {
        self.metadata_bit(asm_call_metadata::IS_CLASS_CONSTRUCTOR)
    }

    pub fn constructor_kind(&self) -> ConstructorKind {
        if self.metadata_bit(asm_call_metadata::IS_DERIVED_CONSTRUCTOR) {
            ConstructorKind::Derived
        } else {
            ConstructorKind::Base
        }
    }

    pub fn set_constructor_kind(&self, constructor_kind: ConstructorKind) {
        self.set_metadata_bit(
            asm_call_metadata::IS_DERIVED_CONSTRUCTOR,
            constructor_kind == ConstructorKind::Derived,
        );
    }

    pub fn uses_this(&self) -> bool {
        self.metadata_bit(asm_call_metadata::USES_THIS)
    }

    pub fn set_uses_this(&self, uses_this: bool) {
        self.set_metadata_bit(asm_call_metadata::USES_THIS, uses_this);
    }

    pub fn this_value_needs_environment_resolution(&self) -> bool {
        self.metadata_bit(asm_call_metadata::THIS_VALUE_NEEDS_ENVIRONMENT_RESOLUTION)
    }

    /// How many properties to make room for in an object that a construction of the function creates with `shape`:
    /// as many as the objects with the shape ended up with, which the longest chain of forward transitions from the
    /// shape tells (every object a constructor builds the same way follows one). The transitions are looked at again
    /// on the first, second, fourth and so on construction, up to the 1024th.
    pub fn construct_reserve(&self, shape: Gc<Shape>) -> u32 {
        let metadata = self.asm_call_metadata.get();
        let count =
            (metadata >> asm_call_metadata::CONSTRUCT_COUNT_SHIFT & asm_call_metadata::CONSTRUCT_COUNT_MASK) as u32;
        if count <= MAX_CONSTRUCT_RESERVE_UPDATES {
            if count.is_power_of_two() {
                let reserve = shape.property_count() + shape.longest_forward_transition_chain(MAX_CONSTRUCT_RESERVE);
                self.construct_reserve.set(reserve.min(MAX_CONSTRUCT_RESERVE) as u8);
            }
            self.asm_call_metadata
                .set(metadata + (1 << asm_call_metadata::CONSTRUCT_COUNT_SHIFT));
        }
        u32::from(self.construct_reserve.get())
    }

    pub fn closure_count(&self) -> ClosureCount {
        let count = self.asm_call_metadata.get() >> asm_call_metadata::CLOSURE_COUNT_SHIFT
            & asm_call_metadata::CLOSURE_COUNT_MASK;
        match count {
            0 => ClosureCount::None,
            1 => ClosureCount::One,
            _ => ClosureCount::Many,
        }
    }

    /// Counts a new closure of the function.
    pub fn note_closure_created(&self) {
        let count = match self.closure_count() {
            ClosureCount::None => ClosureCount::One,
            ClosureCount::One | ClosureCount::Many => ClosureCount::Many,
        };
        let metadata = self.asm_call_metadata.get()
            & !(asm_call_metadata::CLOSURE_COUNT_MASK << asm_call_metadata::CLOSURE_COUNT_SHIFT);
        self.asm_call_metadata
            .set(metadata | (count as u64) << asm_call_metadata::CLOSURE_COUNT_SHIFT);
    }

    /// The reserve of the last construct_reserve(), without asking for it.
    pub fn current_construct_reserve(&self) -> u32 {
        u32::from(self.construct_reserve.get())
    }

    /// Whether the environment of a call has to answer for `this`, `new.target` or `super`, because something reads
    /// them through the environment. Otherwise a declarative environment does for the function's bindings, just as no
    /// environment at all does for a function without any.
    pub fn function_environment_has_this_binding(&self) -> bool {
        let metadata = self.asm_call_metadata.get();
        metadata & (asm_call_metadata::CONTAINS_DIRECT_CALL_TO_EVAL | asm_call_metadata::IS_MODULE_WRAPPER) != 0
            || metadata
                & (asm_call_metadata::IS_ARROW_FUNCTION | asm_call_metadata::THIS_VALUE_NEEDS_ENVIRONMENT_RESOLUTION)
                == asm_call_metadata::THIS_VALUE_NEEDS_ENVIRONMENT_RESOLUTION
    }

    pub fn function_environment_needed(&self) -> bool {
        self.metadata_bit(asm_call_metadata::FUNCTION_ENVIRONMENT_NEEDED)
    }

    pub fn set_function_environment_needed(&self, function_environment_needed: bool) {
        self.set_metadata_bit(
            asm_call_metadata::FUNCTION_ENVIRONMENT_NEEDED,
            function_environment_needed,
        );
    }

    pub fn might_need_arguments_object(&self) -> bool {
        self.metadata_bit(asm_call_metadata::MIGHT_NEED_ARGUMENTS_OBJECT)
    }

    pub fn contains_direct_call_to_eval(&self) -> bool {
        self.metadata_bit(asm_call_metadata::CONTAINS_DIRECT_CALL_TO_EVAL)
    }

    pub fn function_environment_bindings_count(&self) -> usize {
        self.storage.function_environment_bindings_count.get() as usize
    }

    pub fn var_environment_bindings_count(&self) -> usize {
        self.storage
            .rare_data
            .get()
            .map_or(0, |rare_data| rare_data.var_environment_bindings_count.get() as usize)
    }

    /// Where the function environments of calls to the function find their shape.
    pub fn function_environment_shape_cache(&self) -> EnvironmentShapeCache {
        EnvironmentShapeCache::FunctionEnvironment(Gc::from_ref(self))
    }

    /// Where the separate var environments of calls to the function find their shape.
    pub fn var_environment_shape_cache(&self) -> EnvironmentShapeCache {
        EnvironmentShapeCache::VarEnvironment(Gc::from_ref(self))
    }

    pub(crate) fn function_environment_shape_slot(&self) -> &NullableMember<EnvironmentShape> {
        &self.storage.function_environment_shape
    }

    pub(crate) fn var_environment_shape_slot(&self) -> &NullableMember<EnvironmentShape> {
        &self.rare_data().var_environment_shape
    }

    /// RustIntegration::compile_function(): the function's executable, from the bytecode compiled ahead of time if
    /// there is some, and otherwise compiled from its AST now.
    pub fn compile_function(
        vm: &Vm,
        shared_data: Gc<SharedFunctionInstanceData>,
        builtin_abstract_operations_enabled: bool,
    ) -> Option<Gc<Executable>> {
        let source_code = shared_data.source_code();

        let payload = match shared_data.storage.compile_input.take() {
            CompileInput::PrecompiledBytecode(precompiled) => {
                return Some(Executable::create_with_source_code(
                    vm,
                    precompiled.executable,
                    source_code.as_ref(),
                ));
            }
            CompileInput::CachedBytecode(cached_executable) => {
                let source_code = source_code.expect("a function of a bytecode cache blob has its source code");
                return materialize_cached_function_executable(vm, &cached_executable, &source_code);
            }
            CompileInput::FunctionAst(payload) => payload,
            CompileInput::None => panic!("a function that was not compiled yet has its AST"),
        };
        // NB: The frontend checks that nested functions lie within the source, which is unknown without a source code.
        let source_length = source_code
            .as_ref()
            .map_or(usize::MAX, |source_code| source_code.length_in_code_units());
        let precompiled = crate::compile::compile_function(
            payload,
            source_length,
            builtin_abstract_operations_enabled,
            FunctionPrecompileMode::EagerOnly,
        );

        shared_data.set_metadata(&precompiled.metadata);

        Some(Executable::create_with_source_code(
            vm,
            precompiled.executable,
            source_code.as_ref(),
        ))
    }
}
