/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, RefCell};
use std::rc::Rc;

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

pub use crate::ast::FunctionKind;
use crate::ast::FunctionPayload;
use crate::bytecode::bytecode_cache::materialize_cached_function_executable;
use crate::bytecode::executable::Executable;
use crate::bytecode::executable_data::ExecutableData;
use crate::bytecode::generator::{FunctionSfdMetadata, PendingSharedFunctionData, PrecompiledFunction};
use crate::bytecode_cache::{DecodedCachedExecutableRecord, DecodedFunctionRecord};
use crate::compile::FunctionPrecompileMode;
use crate::compile::SharedFunctionDescription;
use crate::frontend_host::rust_free_compiled_regex;
use crate::gc::class::{GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::run::should_dump_bytecode;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
pub use crate::layout::function_object::{SharedFunctionInstanceData, asm_call_metadata};
use crate::runtime::environment_shape::{EnvironmentShape, EnvironmentShapeCache};
use crate::runtime::private_environment::PrivateName;
use crate::runtime::property_key::PropertyKey;
use crate::source_code::SourceCode;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ThisMode {
    Lexical,
    Strict,
    Global,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ConstructorKind {
    Base,
    Derived,
}

/// [[ClassFieldInitializerName]]: Variant<PropertyKey, PrivateName, Empty>.
#[derive(Clone, Debug, Default, Trace)]
pub enum ClassFieldInitializerName {
    PropertyKey(PropertyKey),
    PrivateName(#[gc(untraced)] PrivateName),
    #[default]
    Empty,
}

/// The parts of the shared data of a function that the interpreter does not read.
#[derive(Trace)]
pub struct SharedFunctionInstanceDataStorage {
    name: Utf16FlyString,

    // NB: source_text_offset and source_text_length normally refer to
    //     ranges within the underlying SourceCode we parsed the AST from,
    //     kept alive by source_code. source_text_owner is used if the
    //     source text needs to be owned by the function data (e.g. for
    //     dynamically created functions via Function constructor).
    #[gc(untraced)]
    source_code: RefCell<Option<Rc<SourceCode>>>,
    source_text_owner: GcRefCell<Utf16String>,
    source_text_offset: Cell<usize>,
    source_text_length: Cell<usize>, // [[SourceText]]

    function_length: i32,
    parameter_names_for_mapped_arguments: Vec<Utf16FlyString>,

    #[gc(untraced)]
    this_mode: ThisMode, // [[ThisMode]]
    #[gc(untraced)]
    kind: FunctionKind,

    might_need_arguments_object: Cell<bool>,
    contains_direct_call_to_eval: Cell<bool>,
    is_arrow_function: bool,
    has_simple_parameter_list: bool,
    is_module_wrapper: Cell<bool>,

    this_value_needs_environment_resolution: Cell<bool>,

    function_environment_bindings_count: Cell<usize>,
    var_environment_bindings_count: Cell<usize>,
    function_environment_shape: Cell<Option<Gc<EnvironmentShape>>>,
    var_environment_shape: Cell<Option<Gc<EnvironmentShape>>>,

    class_field_initializer_name: GcRefCell<ClassFieldInitializerName>, // [[ClassFieldInitializerName]]
    #[gc(untraced)]
    constructor_kind: Cell<ConstructorKind>,         // [[ConstructorKind]]
    is_class_constructor: Cell<bool>,                                   // [[IsClassConstructor]]

    // NB: When present, the function's AST, which its bytecode is compiled from when it is first called.
    #[gc(untraced)]
    rust_function_ast: RefCell<Option<Box<FunctionPayload>>>,
    // NB: When present, freshly compiled bytecode that becomes the function's executable when it is first called.
    #[gc(untraced)]
    precompiled_bytecode_executable: RefCell<Option<Box<PrecompiledFunction>>>,
    // NB: When present, the function's executable in a bytecode cache blob, which becomes its executable when it is
    //     first called.
    #[gc(untraced)]
    cached_bytecode_executable: RefCell<Option<Box<DecodedCachedExecutableRecord>>>,
    // NB: The source text range the function was created with, which a bytecode cache blob identifies the function
    //     by, even after the range of a class constructor grew to the whole class.
    bytecode_cache_source_text_offset: Cell<usize>,
    bytecode_cache_source_text_length: Cell<usize>,
    use_rust_compilation: bool,
}

impl Drop for SharedFunctionInstanceDataStorage {
    fn drop(&mut self) {
        if let Some(precompiled) = self.precompiled_bytecode_executable.get_mut().take() {
            discard_precompiled_function(precompiled);
        }
    }
}

/// Frees bytecode that the frontend compiled ahead of a call that never came, along with the regular expressions the
/// host compiled for it and for the functions nested in it, which only an executable created from it frees otherwise.
pub fn discard_precompiled_function(mut precompiled: Box<PrecompiledFunction>) {
    fn free_compiled_regexes(executable: &mut ExecutableData) {
        for regex in executable.compiled_regexes.drain(..) {
            // SAFETY: No executable was created from this bytecode, so it still owns the regexes, which are freed once.
            unsafe { rust_free_compiled_regex(regex.into_raw()) };
        }
        for nested in &mut executable.shared_function_data {
            if let Some(nested_precompiled) = &mut nested.precompiled_function {
                free_compiled_regexes(&mut nested_precompiled.executable);
            }
        }
    }
    free_compiled_regexes(&mut precompiled.executable);
}

define_cell!(SharedFunctionInstanceData, Other);

// SAFETY: Visits the executable and every cell the storage holds.
unsafe impl Trace for SharedFunctionInstanceData {
    fn trace(&self, visitor: &mut Visitor) {
        self.executable.trace(visitor);
        self.storage.trace(visitor);
    }
}

impl SharedFunctionInstanceData {
    #[allow(clippy::too_many_arguments)]
    fn new(
        kind: FunctionKind,
        name: Utf16FlyString,
        function_length: i32,
        formal_parameter_count: u32,
        strict: bool,
        is_arrow_function: bool,
        has_simple_parameter_list: bool,
        parameter_names_for_mapped_arguments: Vec<Utf16FlyString>,
        rust_function_ast: Option<Box<FunctionPayload>>,
    ) -> Self {
        // NB: initialize_after_construction()
        let this_mode = if is_arrow_function {
            ThisMode::Lexical
        } else if strict {
            ThisMode::Strict
        } else {
            ThisMode::Global
        };

        Self {
            header: CellHeader::for_class(Self::CLASS),
            executable: Cell::new(None),
            asm_call_metadata: Cell::new(0),
            formal_parameter_count: Cell::new(formal_parameter_count),
            strict: Cell::new(strict),
            function_environment_needed: Cell::new(false),
            uses_this: Cell::new(false),
            can_inline_call: Cell::new(false),
            storage: SharedFunctionInstanceDataStorage {
                name,
                source_code: RefCell::new(None),
                source_text_owner: GcRefCell::new(Utf16String::default()),
                source_text_offset: Cell::new(0),
                source_text_length: Cell::new(0),
                function_length,
                parameter_names_for_mapped_arguments,
                this_mode,
                kind,
                might_need_arguments_object: Cell::new(true),
                contains_direct_call_to_eval: Cell::new(true),
                is_arrow_function,
                has_simple_parameter_list,
                is_module_wrapper: Cell::new(false),
                this_value_needs_environment_resolution: Cell::new(false),
                function_environment_bindings_count: Cell::new(0),
                var_environment_bindings_count: Cell::new(0),
                function_environment_shape: Cell::new(None),
                var_environment_shape: Cell::new(None),
                class_field_initializer_name: GcRefCell::new(ClassFieldInitializerName::Empty),
                constructor_kind: Cell::new(ConstructorKind::Base),
                is_class_constructor: Cell::new(false),
                rust_function_ast: RefCell::new(rust_function_ast),
                precompiled_bytecode_executable: RefCell::new(None),
                cached_bytecode_executable: RefCell::new(None),
                bytecode_cache_source_text_offset: Cell::new(0),
                bytecode_cache_source_text_length: Cell::new(0),
                use_rust_compilation: true,
            },
        }
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

        let shared = vm.heap().allocate(Self::new(
            description.function_kind,
            name,
            description.function_length,
            description.formal_parameter_count,
            description.strict,
            description.is_arrow,
            description.has_simple_parameter_list,
            mapped_parameter_names,
            Some(description.payload),
        ));
        shared.update_can_inline_call();

        // Set parsing insights that must be available before lazy compilation.
        shared.uses_this.set(description.uses_this);
        shared
            .storage
            .this_value_needs_environment_resolution
            .set(description.uses_this_from_environment);
        if description.uses_this_from_environment && !description.is_arrow {
            shared.function_environment_needed.set(true);
        }
        shared.update_asm_call_metadata();

        if let Some(source_code) = source_code {
            shared.set_source_text_range(
                source_code,
                description.source_text_offset,
                description.source_text_length,
            );
        }
        shared.set_bytecode_cache_source_text_range(description.source_text_offset, description.source_text_length);
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

        let shared = vm.heap().allocate(Self::new(
            function.function_kind(),
            name,
            function.length(),
            function.parameter_count(),
            function.has_strict_code() || outer_strict,
            is_arrow_function,
            has_simple_parameter_list,
            simple_parameter_names.unwrap_or_default(),
            None,
        ));
        shared.update_can_inline_call();

        // Set parsing insights that must be available before lazy compilation.
        let (uses_this, uses_this_from_environment) = function.parsing_insights_about_this();
        shared.uses_this.set(uses_this);
        shared
            .storage
            .this_value_needs_environment_resolution
            .set(uses_this_from_environment);
        if uses_this_from_environment && !is_arrow_function {
            shared.function_environment_needed.set(true);
        }
        shared.update_asm_call_metadata();

        shared.set_source_text_range(source_code, source_text_range.start, source_text_length);
        shared.set_bytecode_cache_source_text_range(source_text_range.start, source_text_length);
        if let Some((name, is_private)) = function.field_initializer_name() {
            shared.set_class_field_initializer_name_from_utf16(&name, is_private);
        }
        shared.set_metadata(function.scope_metadata());
        shared
            .storage
            .cached_bytecode_executable
            .replace(Some(Box::new(function.cached_executable())));
        shared
    }

    fn set_bytecode_cache_source_text_range(&self, source_text_offset: usize, source_text_length: usize) {
        self.storage.bytecode_cache_source_text_offset.set(source_text_offset);
        self.storage.bytecode_cache_source_text_length.set(source_text_length);
    }

    /// The offset and length of the source text the function was created with.
    pub fn bytecode_cache_source_text_range(&self) -> (usize, usize) {
        (
            self.storage.bytecode_cache_source_text_offset.get(),
            self.storage.bytecode_cache_source_text_length.get(),
        )
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
            && self.storage.function_length == function.length()
            && self.formal_parameter_count.get() == function.parameter_count()
            && self.storage.kind == function.function_kind()
            && self.strict.get() == (function.has_strict_code() || outer_strict)
            && self.storage.is_arrow_function == function.is_arrow()
            && self.storage.has_simple_parameter_list == function.simple_parameter_names().is_some()
    }

    /// Replaces how the function compiles with its executable in a bytecode cache blob, which becomes its executable
    /// when it is first called, as rust_sfd_install_cached_bytecode_executable does.
    pub fn install_cached_bytecode_executable(
        &self,
        cached_executable: DecodedCachedExecutableRecord,
        metadata: &FunctionSfdMetadata,
    ) {
        self.clear_compile_inputs();
        self.set_metadata(metadata);
        self.storage
            .cached_bytecode_executable
            .replace(Some(Box::new(cached_executable)));
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
        let shared = vm.heap().allocate(Self::new(
            kind,
            name,
            function_length,
            formal_parameter_count,
            strict,
            is_arrow_function,
            has_simple_parameter_list,
            parameter_names_for_mapped_arguments,
            None,
        ));
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
        self.storage.is_class_constructor.set(true);
        self.update_can_inline_call();
    }

    pub fn update_asm_call_metadata(&self) {
        let mut metadata = u64::from(self.formal_parameter_count.get());
        if self.can_inline_call.get() {
            metadata |= asm_call_metadata::CAN_INLINE_CALL;
        }
        if self.function_environment_needed.get() || self.storage.this_value_needs_environment_resolution.get() {
            metadata |= asm_call_metadata::NEEDS_ENVIRONMENT_OR_THIS_VALUE_RESOLUTION;
        }
        if self.uses_this.get() {
            metadata |= asm_call_metadata::USES_THIS;
        }
        if self.strict.get() {
            metadata |= asm_call_metadata::STRICT;
        }
        self.asm_call_metadata.set(metadata);
    }

    pub fn can_inline_call(&self) -> bool {
        self.can_inline_call.get()
    }

    fn update_can_inline_call(&self) {
        self.can_inline_call.set(
            self.executable.get().is_some()
                && self.storage.kind == FunctionKind::Normal
                && !self.storage.is_class_constructor.get(),
        );
        self.update_asm_call_metadata();
    }

    pub fn name(&self) -> Utf16FlyString {
        self.storage.name.clone()
    }

    pub fn source_text(&self) -> Utf16String {
        {
            let source_text_owner = self.storage.source_text_owner.borrow();
            if !source_text_owner.is_empty() {
                return source_text_owner.clone();
            }
        }

        let Some(source_code) = self.storage.source_code.borrow().clone() else {
            return Utf16String::default();
        };

        let source_text = source_code.source_text_from_offsets(
            self.storage.source_text_offset.get(),
            self.storage.source_text_length.get(),
        );
        self.storage.source_text_owner.replace(source_text.clone());
        source_text
    }

    /// Makes `source_text` the [[SourceText]] while keeping the source code the function compiles from.
    pub fn set_source_text_owner(&self, source_text: Utf16String) {
        self.storage.source_text_owner.replace(source_text);
    }

    pub fn set_source_text(&self, source_text: Utf16String) {
        self.storage.source_text_owner.replace(source_text);
        self.storage.source_code.replace(None);
        self.storage.source_text_offset.set(0);
        self.storage.source_text_length.set(0);
    }

    pub fn set_source_text_range(
        &self,
        source_code: &Rc<SourceCode>,
        source_text_offset: usize,
        source_text_length: usize,
    ) {
        self.storage.source_text_owner.replace(Utf16String::default());
        self.storage.source_code.replace(Some(source_code.clone()));
        self.storage.source_text_offset.set(source_text_offset);
        self.storage.source_text_length.set(source_text_length);
    }

    pub fn source_code(&self) -> Option<Rc<SourceCode>> {
        self.storage.source_code.borrow().clone()
    }

    pub fn clear_compile_inputs(&self) {
        self.clear_non_bytecode_cache_compile_inputs();
        drop(self.storage.cached_bytecode_executable.take());
    }

    pub fn clear_non_bytecode_cache_compile_inputs(&self) {
        drop(self.storage.rust_function_ast.take());
        if let Some(precompiled) = self.storage.precompiled_bytecode_executable.take() {
            discard_precompiled_function(precompiled);
        }
    }

    /// Whether the function still has the AST it compiles from when it is first called.
    pub fn has_function_ast(&self) -> bool {
        self.storage.rust_function_ast.borrow().is_some()
    }

    /// Whether the function still has bytecode compiled ahead of its first call, apart from that of a bytecode cache.
    pub fn has_precompiled_bytecode(&self) -> bool {
        self.storage.precompiled_bytecode_executable.borrow().is_some()
    }

    /// Whether the function's executable is still in a bytecode cache blob, waiting for its first call.
    pub fn has_cached_bytecode(&self) -> bool {
        self.storage.cached_bytecode_executable.borrow().is_some()
    }

    /// The functions that `executable` creates and that have not been compiled yet, with a copy of the AST of each, so
    /// that they can be compiled elsewhere, such as on another thread, while their own ASTs stay in place for a first
    /// call that comes sooner.
    pub fn uncompiled_functions_of(
        executable: Gc<Executable>,
    ) -> Vec<(Gc<SharedFunctionInstanceData>, Box<FunctionPayload>)> {
        (0..executable.shared_function_data_count())
            .filter_map(|index| {
                let shared_data = executable.shared_function_data(u32::try_from(index).expect("the index fits in u32"));
                if shared_data.executable().is_some() {
                    return None;
                }
                let function_ast = shared_data.storage.rust_function_ast.borrow().clone()?;
                Some((shared_data, function_ast))
            })
            .collect()
    }

    /// Records what scope analysis found out about the function's body, as rust_sfd_set_metadata does.
    pub fn set_metadata(&self, metadata: &FunctionSfdMetadata) {
        self.uses_this.set(metadata.uses_this);
        self.storage
            .this_value_needs_environment_resolution
            .set(metadata.this_value_needs_environment_resolution);
        self.function_environment_needed
            .set(metadata.function_environment_needed);
        self.update_asm_call_metadata();
        self.storage
            .function_environment_bindings_count
            .set(metadata.function_environment_bindings_count);
        self.storage
            .var_environment_bindings_count
            .set(metadata.var_environment_bindings_count);
        self.storage
            .might_need_arguments_object
            .set(metadata.might_need_arguments);
        self.storage.contains_direct_call_to_eval.set(metadata.contains_eval);
    }

    /// Keeps bytecode the frontend compiled ahead of the first call, as rust_sfd_set_precompiled_bytecode_executable
    /// does.
    pub fn set_precompiled_bytecode_executable(&self, precompiled: Box<PrecompiledFunction>) {
        self.clear_compile_inputs();
        self.set_metadata(&precompiled.metadata);
        self.storage.precompiled_bytecode_executable.replace(Some(precompiled));
    }

    /// Sets [[ClassFieldInitializerName]] to a name the frontend found, as rust_sfd_set_class_field_initializer_name
    /// does.
    pub fn set_class_field_initializer_name_from_utf16(&self, name: &[u16], is_private: bool) {
        let name = Utf16FlyString::from_utf16(name);
        self.set_class_field_initializer_name(if is_private {
            ClassFieldInitializerName::PrivateName(PrivateName::new(0, name))
        } else {
            ClassFieldInitializerName::PropertyKey(PropertyKey::from(name))
        });
    }

    pub fn class_field_initializer_name(&self) -> ClassFieldInitializerName {
        self.storage.class_field_initializer_name.borrow().clone()
    }

    pub fn set_class_field_initializer_name(&self, name: ClassFieldInitializerName) {
        self.storage.class_field_initializer_name.replace(name);
    }

    pub fn kind(&self) -> FunctionKind {
        self.storage.kind
    }

    pub fn this_mode(&self) -> ThisMode {
        self.storage.this_mode
    }

    pub fn function_length(&self) -> i32 {
        self.storage.function_length
    }

    pub fn formal_parameter_count(&self) -> u32 {
        self.formal_parameter_count.get()
    }

    pub fn parameter_names_for_mapped_arguments(&self) -> &[Utf16FlyString] {
        &self.storage.parameter_names_for_mapped_arguments
    }

    pub fn strict(&self) -> bool {
        self.strict.get()
    }

    pub fn is_arrow_function(&self) -> bool {
        self.storage.is_arrow_function
    }

    pub fn has_simple_parameter_list(&self) -> bool {
        self.storage.has_simple_parameter_list
    }

    pub fn is_module_wrapper(&self) -> bool {
        self.storage.is_module_wrapper.get()
    }

    pub fn set_is_module_wrapper(&self, is_module_wrapper: bool) {
        self.storage.is_module_wrapper.set(is_module_wrapper);
    }

    pub fn is_class_constructor(&self) -> bool {
        self.storage.is_class_constructor.get()
    }

    pub fn constructor_kind(&self) -> ConstructorKind {
        self.storage.constructor_kind.get()
    }

    pub fn set_constructor_kind(&self, constructor_kind: ConstructorKind) {
        self.storage.constructor_kind.set(constructor_kind);
    }

    pub fn uses_this(&self) -> bool {
        self.uses_this.get()
    }

    pub fn this_value_needs_environment_resolution(&self) -> bool {
        self.storage.this_value_needs_environment_resolution.get()
    }

    pub fn function_environment_needed(&self) -> bool {
        self.function_environment_needed.get()
    }

    pub fn might_need_arguments_object(&self) -> bool {
        self.storage.might_need_arguments_object.get()
    }

    pub fn contains_direct_call_to_eval(&self) -> bool {
        self.storage.contains_direct_call_to_eval.get()
    }

    pub fn function_environment_bindings_count(&self) -> usize {
        self.storage.function_environment_bindings_count.get()
    }

    pub fn var_environment_bindings_count(&self) -> usize {
        self.storage.var_environment_bindings_count.get()
    }

    /// Where the function environments of calls to the function find their shape.
    pub fn function_environment_shape_cache(&self) -> EnvironmentShapeCache {
        // SAFETY: The slot is part of this shared data, which only exists as a cell.
        unsafe { EnvironmentShapeCache::new(Gc::from_ref(self), &self.storage.function_environment_shape) }
    }

    /// Where the separate var environments of calls to the function find their shape.
    pub fn var_environment_shape_cache(&self) -> EnvironmentShapeCache {
        // SAFETY: As above.
        unsafe { EnvironmentShapeCache::new(Gc::from_ref(self), &self.storage.var_environment_shape) }
    }

    /// RustIntegration::compile_function(): the function's executable, from the bytecode compiled ahead of time if
    /// there is some, and otherwise compiled from its AST now.
    pub fn compile_function(
        vm: &Vm,
        shared_data: Gc<SharedFunctionInstanceData>,
        builtin_abstract_operations_enabled: bool,
    ) -> Option<Gc<Executable>> {
        let source_code = shared_data.source_code();

        if let Some(precompiled) = shared_data.storage.precompiled_bytecode_executable.take() {
            return Some(Executable::create_with_source_code(
                vm,
                precompiled.executable,
                source_code.as_ref(),
            ));
        }

        if let Some(cached_executable) = shared_data.storage.cached_bytecode_executable.take() {
            let source_code = source_code.expect("a function of a bytecode cache blob has its source code");
            return materialize_cached_function_executable(vm, &cached_executable, &source_code);
        }

        if !shared_data.storage.use_rust_compilation {
            return None;
        }

        let payload = shared_data
            .storage
            .rust_function_ast
            .take()
            .expect("a function that was not compiled yet has its AST");
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
