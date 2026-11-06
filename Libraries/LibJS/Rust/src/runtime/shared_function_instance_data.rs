/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, OnceCell, RefCell};
use std::collections::HashSet;
use std::rc::Rc;
use std::sync::Arc;

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

pub use crate::ast::FunctionKind;
use crate::bytecode::executable::Executable;
use crate::bytecode::executable_data::{FunctionSfdMetadata, PendingSharedFunctionData, PrecompiledFunction};
use crate::compile::FunctionPayload;
use crate::compile::SharedFunctionDescription;
use crate::gc::class::{GcCell, define_cell};
use crate::gc::defer_gc::DeferGc;
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeaderTail, Gc};
use crate::layout::execution_context::{ScriptOrModule, ScriptOrModuleSlot};
use crate::layout::function_object::CallEnvironmentTemplate;
pub use crate::layout::function_object::{SharedFunctionInstanceData, asm_call_metadata};
use crate::layout::member::NullableMember;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::environment_shape::{EnvironmentShape, EnvironmentShapeCache};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
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
    /// The function is not parsed yet: its source and its row there, which its tree is parsed from.
    Unparsed(Arc<FunctionPayload>),
    /// Freshly compiled bytecode.
    PrecompiledBytecode(Arc<PrecompiledFunction>),
}

/// A CompileInput in one word: the pointer, with what it points at in its lowest bits.
struct CompileInputSlot(Cell<*mut ()>);

// NB: The pointers in a slot leave room for its tag.
const _: () = assert!(
    align_of::<FunctionPayload>() > CompileInputSlot::TAG_MASK
        && align_of::<PrecompiledFunction>() > CompileInputSlot::TAG_MASK
);

impl CompileInputSlot {
    const TAG_MASK: usize = 0b11;
    const TAG_UNPARSED: usize = 1;
    const TAG_PRECOMPILED_BYTECODE: usize = 2;

    fn new(input: CompileInput) -> Self {
        let slot = Self(Cell::new(core::ptr::null_mut()));
        slot.set(input);
        slot
    }

    fn set(&self, input: CompileInput) {
        drop(self.take());
        self.0.set(match input {
            CompileInput::None => core::ptr::null_mut(),
            CompileInput::Unparsed(payload) => Self::tagged(Arc::into_raw(payload).cast_mut(), Self::TAG_UNPARSED),
            CompileInput::PrecompiledBytecode(precompiled) => {
                Self::tagged(Arc::into_raw(precompiled).cast_mut(), Self::TAG_PRECOMPILED_BYTECODE)
            }
        });
    }

    fn tagged<T>(pointer: *mut T, tag: usize) -> *mut () {
        pointer.cast::<()>().map_addr(|address| address | tag)
    }

    fn take(&self) -> CompileInput {
        let word = self.0.replace(core::ptr::null_mut());
        let pointer = word.map_addr(|address| address & !Self::TAG_MASK);
        // SAFETY: set() made the word from a reference of the type its tag says, which this takes over now that the
        //         word is gone from the slot.
        unsafe {
            match word.addr() & Self::TAG_MASK {
                Self::TAG_UNPARSED => CompileInput::Unparsed(Arc::from_raw(pointer.cast())),
                Self::TAG_PRECOMPILED_BYTECODE => CompileInput::PrecompiledBytecode(Arc::from_raw(pointer.cast())),
                _ => CompileInput::None,
            }
        }
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
    /// Functions with parameter expressions may have a separate var environment.
    var_environment_bindings_count: Cell<u32>,
    var_environment_shape: NullableMember<EnvironmentShape>,
}

/// The parts of the shared data of a function that the interpreter does not read.
#[derive(Trace)]
pub struct SharedFunctionInstanceDataStorage {
    name: Utf16FlyString,

    // NB: source_text_offset and source_text_length are a range of the source code we parsed the function from.
    #[gc(untraced)]
    source_code: Cell<Option<Rc<SourceCode>>>,
    source_text_offset: Cell<u32>,
    source_text_length: Cell<u32>, // [[SourceText]]

    #[gc(untraced)]
    parameter_names: OnceCell<Box<ParameterNames>>,
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
fn parameter_names_of(parameter_names: &[Utf16FlyString]) -> Option<Box<ParameterNames>> {
    if parameter_names.is_empty() {
        return None;
    }
    // OPTIMIZATION: Going through a few names beats a hash set of them.
    const MAXIMUM_PARAMETERS_TO_SCAN: usize = 8;
    if parameter_names.len() <= MAXIMUM_PARAMETERS_TO_SCAN {
        let has_repeated_name =
            (1..parameter_names.len()).any(|index| parameter_names[..index].contains(&parameter_names[index]));
        if !has_repeated_name {
            let names: Rc<[Utf16FlyString]> = parameter_names.into();
            return Some(Box::new(ParameterNames {
                mapped_argument_names: names.clone(),
                parameter_binding_names: names,
            }));
        }
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
        for name in parameter_names {
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
        payload: Option<Arc<FunctionPayload>>,
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
                parameter_names: parameter_names_of(&parameter_names_for_mapped_arguments)
                    .map_or_else(OnceCell::new, OnceCell::from),
                compile_input: CompileInputSlot::new(payload.map_or(CompileInput::None, CompileInput::Unparsed)),
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

    /// Creates the shared data of a function the frontend described, which is strict if `strict`.
    pub fn create(
        vm: &Vm,
        description: &SharedFunctionDescription,
        strict: bool,
        source_code: Option<&Rc<SourceCode>>,
    ) -> Gc<SharedFunctionInstanceData> {
        Self::create_from_owned_description(vm, description.clone(), strict, source_code)
    }

    fn create_from_owned_description(
        vm: &Vm,
        description: SharedFunctionDescription,
        strict: bool,
        source_code: Option<&Rc<SourceCode>>,
    ) -> Gc<SharedFunctionInstanceData> {
        let SharedFunctionDescription {
            name,
            function_kind,
            function_length,
            formal_parameter_count,
            is_arrow,
            has_simple_parameter_list,
            source_text_offset,
            source_text_length,
            uses_this,
            uses_this_from_environment,
            payload,
            ..
        } = description;
        let shared = vm.heap().allocate(|defer| {
            Self::new(
                defer,
                function_kind,
                name,
                function_length,
                formal_parameter_count,
                strict,
                is_arrow,
                has_simple_parameter_list,
                Vec::new(),
                Some(payload),
            )
        });
        shared.update_can_inline_call();

        // Set parsing insights that must be available before lazy compilation.
        shared.set_uses_this(uses_this);
        shared.set_metadata_bit(
            asm_call_metadata::THIS_VALUE_NEEDS_ENVIRONMENT_RESOLUTION,
            uses_this_from_environment,
        );
        if uses_this_from_environment && !is_arrow {
            shared.set_function_environment_needed(true);
        }

        shared.storage.source_code.set(source_code.cloned());
        shared.initialize_source_text_range(source_text_offset, source_text_length);
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

    /// vm.heap().allocate<SharedFunctionInstanceData>(vm, kind, name, function_length, formal_parameter_count, strict,
    /// is_arrow_function, has_simple_parameter_list, parameter_names_for_mapped_arguments, NoSharedFunctionDataList {},
    /// nullptr): shared data that has nothing to compile, such as the wrapper of a module with top-level await, whose
    /// executable the caller sets.
    #[allow(clippy::too_many_arguments)]
    pub fn create_without_compile_input(
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

    /// Creates the shared data of one of the functions an executable declares, from what the frontend kept for it: its
    /// description, its class field initializer name and the bytecode it may have compiled already.
    pub fn create_from_pending_shared_function_data(
        vm: &Vm,
        pending: &PendingSharedFunctionData,
        is_strict: bool,
        source_code: Option<&Rc<SourceCode>>,
    ) -> Gc<SharedFunctionInstanceData> {
        let shared = Self::create(vm, &pending.description, pending.is_strict_in(is_strict), source_code);
        if let Some((name, is_private)) = &pending.class_field_initializer_name {
            shared.set_class_field_initializer_name_from_utf16(&name.to_utf16(), *is_private);
        }
        if let Some(precompiled) = &pending.precompiled_function {
            shared.set_precompiled_bytecode_executable(precompiled.clone());
        }
        shared
    }

    /// create_from_pending_shared_function_data() for what nothing links again, which moves into the shared data.
    pub fn create_from_owned_pending_shared_function_data(
        vm: &Vm,
        pending: PendingSharedFunctionData,
        is_strict: bool,
        source_code: Option<&Rc<SourceCode>>,
    ) -> Gc<SharedFunctionInstanceData> {
        let strict = pending.is_strict_in(is_strict);
        let shared = Self::create_from_owned_description(vm, pending.description, strict, source_code);
        if let Some((name, is_private)) = &pending.class_field_initializer_name {
            shared.set_class_field_initializer_name_from_utf16(&name.to_utf16(), *is_private);
        }
        if let Some(precompiled) = pending.precompiled_function {
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

    /// Records what scope analysis found out about the function's body, as rust_sfd_set_metadata does.
    pub fn set_metadata(&self, metadata: &FunctionSfdMetadata) {
        if let Some(parameter_names) = parameter_names_of(&metadata.parameter_names) {
            let _ = self.storage.parameter_names.set(parameter_names);
        }
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
    pub fn set_precompiled_bytecode_executable(&self, precompiled: Arc<PrecompiledFunction>) {
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
        self.storage.parameter_names.get().map_or_else(
            || Vm::of_cell(self).no_parameter_names.clone(),
            |names| names.mapped_argument_names.clone(),
        )
    }

    pub fn parameter_binding_names(&self) -> Rc<[Utf16FlyString]> {
        self.storage.parameter_names.get().map_or_else(
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
    /// there is some, and otherwise parsed and compiled now. Running out of stack on the way is a RangeError, like
    /// running out of stack in the call that got here.
    pub fn compile_function(
        vm: &Vm,
        shared_data: Gc<SharedFunctionInstanceData>,
        builtin_abstract_operations_enabled: bool,
    ) -> ThrowCompletionOr<Gc<Executable>> {
        let source_code = shared_data.source_code();

        let payload = match shared_data.storage.compile_input.take() {
            CompileInput::PrecompiledBytecode(precompiled) => {
                // NB: This stays the only reference to the code if nothing else holds the function.
                let executable = Arc::try_unwrap(precompiled)
                    .map_or_else(|shared| shared.executable.clone(), |unique| unique.executable);
                return Ok(Executable::create_with_source_code(
                    vm,
                    executable,
                    source_code.as_ref(),
                ));
            }
            CompileInput::Unparsed(payload) => payload,
            CompileInput::None => panic!("a function that was not compiled yet knows how to compile"),
        };
        let precompiled =
            crate::compile::compile_function(&payload, builtin_abstract_operations_enabled, vm.native_stack_limit());
        let Ok(precompiled) = precompiled else {
            shared_data.storage.compile_input.set(CompileInput::Unparsed(payload));
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::CallStackSizeExceeded, &[]);
        };

        shared_data.set_metadata(&precompiled.metadata);

        Ok(Executable::create_with_source_code(
            vm,
            precompiled.executable,
            source_code.as_ref(),
        ))
    }
}
