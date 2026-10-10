/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Snapshots of executables for compile jobs, captured on the main thread straight from the runtime's structures.
//! A snapshot refers to cells only by address; the cells it captured are kept alive by whoever owns the job.

use core::mem::offset_of;
use libjs_abi::Builtin;
use std::collections::HashMap;

use libjs_jit::bytecode::{ExceptionHandler, FrameLayout};
use libjs_jit::snapshot::{
    AccessorFunctionSnapshot, CallFeedbackSnapshot, CellId, ClosureTemplateSnapshot, CompileOptions, ConstructTarget,
    DirectCallTarget, EnvironmentTemplateSnapshot, ExecutableSnapshot, FeedbackSnapshot, ForwardedCallSnapshot,
    Forwarding, FunctionFrameFields, InlinedFunctionSnapshot, InliningLimits, Intrinsic, KeyedFeedbackSnapshot,
    LexicalEnvironmentTemplateSnapshot, NativeCallTarget, ObjectShapeCacheSnapshot, PropertyCacheEntrySnapshot,
    PropertyCacheEntryType, PropertyCacheKind, PropertyCacheSnapshot, ShapeSnapshot, Snapshot, StressOptions,
};

use super::allocation::{
    allocation_infos, closure_template_snapshots, closure_templates, function_environment_template,
    lexical_environment_templates,
};
use super::cell_as;
use super::code::{InlinedFunction, SnapshotExecutable};
use super::runtime_info::RAW_NATIVE_FUNCTIONS_RETURN_IN_REGISTERS;
use crate::bytecode::executable::{Executable, PropertyLookupCacheEntryData, PropertyLookupCacheTier};
use crate::bytecode::feedback::{CallFeedback, CallFeedbackForwarding, call_feedback_flags};
use crate::bytecode::instruction::instruction_length_from_bytes;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::function_object::{EcmascriptFunctionObject, RawNativeFunction};
use crate::layout::object::Object;
use crate::layout::property_lookup_cache::{
    PropertyLookupCache, PropertyLookupCacheEntry, PropertyLookupCacheEntryType,
};
use crate::layout::value::Value;
use crate::runtime::array_constructor::ArrayConstructor;
use crate::runtime::array_prototype::ArrayPrototype;
use crate::runtime::boolean_constructor::BooleanConstructor;
use crate::runtime::bound_function::BoundFunction;
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;
use crate::runtime::function_prototype::FunctionPrototype;
use crate::runtime::native_javascript_backed_function::NativeJavaScriptBackedFunction;
use crate::runtime::object_constructor::ObjectConstructor;
use crate::runtime::object_prototype::ObjectPrototype;
use crate::runtime::realm::Realm;
use crate::runtime::shape::Shape;
use crate::runtime::shared_function_instance_data::ConstructorKind;
use crate::runtime::shared_function_instance_data::FunctionKind;
use crate::runtime::shared_function_instance_data::ThisMode;
use crate::runtime::string_constructor::StringConstructor;

/// Candidates with at most this many bytecode instructions are always inlined, without counting against the inlining
/// budget.
const ALWAYS_INLINED_INSTRUCTION_COUNT: u32 = 10;

/// Constructors of at most this many instructions are inlined into their constructs (see
/// `InliningLimits::max_construct_instructions`).
const MAX_INLINED_CONSTRUCT_INSTRUCTION_COUNT: u32 = 50;

/// A snapshot with the cells it refers to, which must stay alive (and in place) until the job's code is installed or
/// the job is abandoned.
pub struct CapturedSnapshot {
    pub snapshot: Box<Snapshot>,
    pub cells: Vec<Gc<CellHeader>>,
    pub snapshot_executables: Vec<SnapshotExecutable>,
}

/// An executable of the snapshot.
#[derive(Clone, Copy)]
struct Entry {
    executable: Gc<Executable>,
    /// None for the executable being compiled.
    function: Option<InlinedFunction>,
    realm: Gc<Realm>,
    depth: u32,
}

struct SnapshotBuilder<'vm> {
    vm: &'vm Vm,
    cells: Vec<Gc<CellHeader>>,
    entries: Vec<Entry>,
    indices: HashMap<InlinedFunction, u32>,
    compiled_function: Gc<EcmascriptFunctionObject>,
    /// The closure templates of the compiled executable, until its snapshot takes them. Only the compiled function
    /// allocates closures itself.
    closure_templates: Vec<Option<ClosureTemplateSnapshot>>,
    /// The lexical environment templates of the compiled executable, until its snapshot takes them.
    lexical_environment_templates: Vec<Option<LexicalEnvironmentTemplateSnapshot>>,
    /// The environment templates of the direct calls of the executable whose snapshot is being made.
    environment_templates: Vec<EnvironmentTemplateSnapshot>,
}

/// The number of bytecode instructions of an executable, counted up to one past `limit`.
fn instruction_count(executable: &Executable, limit: u32) -> u32 {
    let bytecode = executable.bytecode();
    let mut count = 0;
    let mut offset = 0;
    while offset < bytecode.len() && count <= limit {
        let Ok(length) = instruction_length_from_bytes(bytecode[offset], bytecode, offset) else {
            break;
        };
        offset += length;
        count += 1;
    }
    count
}

pub(crate) fn ecmascript_function(cell: Option<Gc<CellHeader>>) -> Option<Gc<EcmascriptFunctionObject>> {
    as_ecmascript_function_object(cell_as::<Object>(cell?)?)
}

/// The target of a call site if it is a builtin written in JavaScript that JIT code can inline: a normal function
/// without a function environment of the caller's realm, which already ran.
fn builtin_inlining_candidate(
    target: Option<Gc<CellHeader>>,
    caller_realm: Gc<Realm>,
) -> Option<Gc<NativeJavaScriptBackedFunction>> {
    let builtin = cell_as::<NativeJavaScriptBackedFunction>(target?)?;
    let shared_data = builtin.shared_data();
    (shared_data.kind() == FunctionKind::Normal
        && !builtin.function_environment_needed()
        && shared_data.executable().is_some()
        && builtin.realm() == caller_realm)
        .then_some(builtin)
}

/// The intrinsic the compiler knows an object as, if it is one.
fn intrinsic_of(vm: &Vm, cell: Option<Gc<CellHeader>>) -> Option<Intrinsic> {
    let cell = cell?;
    if cell_as::<StringConstructor>(cell).is_some() {
        return Some(Intrinsic::StringConstructor);
    }
    if cell_as::<ArrayConstructor>(cell).is_some() {
        return Some(Intrinsic::ArrayConstructor);
    }
    if cell_as::<ObjectConstructor>(cell).is_some() {
        return Some(Intrinsic::ObjectConstructor);
    }
    if cell_as::<BooleanConstructor>(cell).is_some() {
        return Some(Intrinsic::BooleanConstructor);
    }
    let function = cell_as::<RawNativeFunction>(cell)?;
    if FunctionPrototype::is_apply_function(vm, &function) {
        Some(Intrinsic::FunctionPrototypeApply)
    } else if FunctionPrototype::is_call_function(vm, &function) {
        Some(Intrinsic::FunctionPrototypeCall)
    } else if ArrayPrototype::is_push_function(vm, &function) {
        Some(Intrinsic::ArrayPrototypePush)
    } else if ArrayPrototype::is_slice_function(vm, &function) {
        Some(Intrinsic::ArrayPrototypeSlice)
    } else if ObjectPrototype::is_has_own_property_function(vm, &function) {
        Some(Intrinsic::ObjectPrototypeHasOwnProperty)
    } else {
        match function.builtin() {
            Some(Builtin::StringFromCharCode) => Some(Intrinsic::StringFromCharCode),
            _ => None,
        }
    }
}

/// The object the holder of a prototype chain lookup cache entry has in the property the entry describes, if the entry
/// is still valid and the property holds an object (such as a method).
fn prototype_property_object(entry: &PropertyLookupCacheEntryData) -> Option<Gc<Object>> {
    if entry.entry_type != PropertyLookupCacheEntryType::GetPropertyInPrototypeChain {
        return None;
    }
    let value = prototype_property_value(entry)?;
    value.is_object().then(|| value.as_object())
}

/// The value the prototype of a GetPropertyInPrototypeChain or ChangePropertyInPrototypeChain entry holds in the
/// property, if the entry still applies.
fn prototype_property_value(entry: &PropertyLookupCacheEntryData) -> Option<Value> {
    if !matches!(
        entry.entry_type,
        PropertyLookupCacheEntryType::GetPropertyInPrototypeChain
            | PropertyLookupCacheEntryType::ChangePropertyInPrototypeChain
    ) {
        return None;
    }
    let holder = entry.prototype?;
    if !entry.prototype_chain_validity?.is_valid() {
        return None;
    }
    if holder.shape().is_dictionary() || entry.property_offset >= holder.shape().property_count() {
        return None;
    }
    Some(holder.get_direct(entry.property_offset))
}

/// The function of the accessor an entry calls, if it is an ECMAScript function: for a GetPropertyInPrototypeChain
/// entry, the getter of the accessor its prototype holds in the property, for a ChangePropertyInPrototypeChain entry
/// its setter, and for a GetOwnProperty entry the getter of the accessor the interpreter last called the getter of.
fn entry_accessor_function(slot: &PropertyLookupCacheEntry) -> Option<Gc<EcmascriptFunctionObject>> {
    let entry = slot.get();
    let (accessor, setter) = match entry.entry_type {
        PropertyLookupCacheEntryType::GetOwnProperty if entry.key == 0 => (slot.accessor.get()?, false),
        entry_type => {
            let value = prototype_property_value(&entry)?;
            if !value.is_accessor() {
                return None;
            }
            (
                value.as_accessor(),
                entry_type == PropertyLookupCacheEntryType::ChangePropertyInPrototypeChain,
            )
        }
    };
    let function = if setter { accessor.setter() } else { accessor.getter() };
    as_ecmascript_function_object(function?)
}

/// Whether the property of an entry held an accessor: the accessor the interpreter last called the getter of for a
/// GetOwnProperty entry, or the value its prototype holds in the property for entries of a prototype.
fn entry_holds_accessor(slot: &PropertyLookupCacheEntry) -> bool {
    let entry = slot.get();
    match entry.entry_type {
        PropertyLookupCacheEntryType::GetOwnProperty if entry.key == 0 => slot.accessor.get().is_some(),
        _ => prototype_property_value(&entry).is_some_and(|value| value.is_accessor()),
    }
}

/// The target of a call site if JIT code can call it directly, building its frame the way the interpreter's call
/// fast path does, and the function environment as libjs_jit_prepare_call_environment() does if it needs one, with
/// whether the site also called other closures of it.
fn direct_call_target(feedback: &CallFeedback) -> Option<(Gc<EcmascriptFunctionObject>, bool)> {
    let flags = feedback.flags.get();
    let closures = flags & call_feedback_flags::POLYMORPHIC != 0;
    if closures && flags & (call_feedback_flags::OTHER_FUNCTIONS | call_feedback_flags::SAW_NATIVE) != 0 {
        return None;
    }
    let function = ecmascript_function(feedback.target())?;
    function.can_inline_call().then_some((function, closures))
}

/// The target of a call site if it is a raw native function that JIT code can call directly, in the frame the
/// interpreter's call fast path builds for it. Function.prototype.call and apply forward their calls, which the runtime
/// does for JIT code.
fn native_call_target(vm: &Vm, feedback: &CallFeedback) -> Option<Gc<RawNativeFunction>> {
    if !RAW_NATIVE_FUNCTIONS_RETURN_IN_REGISTERS || feedback.flags.get() & call_feedback_flags::POLYMORPHIC != 0 {
        return None;
    }
    let function = cell_as::<RawNativeFunction>(feedback.target()?)?;
    (!FunctionPrototype::is_call_function(vm, &function) && !FunctionPrototype::is_apply_function(vm, &function))
        .then_some(function)
}

/// The words of an ECMAScript function its frames start with, which stay the same for its lifetime.
fn function_frame_fields(function: Gc<EcmascriptFunctionObject>) -> FunctionFrameFields {
    FunctionFrameFields {
        script_or_module: [
            word_of(function, offset_of!(EcmascriptFunctionObject, script_or_module)),
            word_of(function, offset_of!(EcmascriptFunctionObject, script_or_module) + 8),
        ],
        environment: word_of(function, offset_of!(EcmascriptFunctionObject, environment)),
        private_environment: word_of(function, offset_of!(EcmascriptFunctionObject, private_environment)),
    }
}

/// The words at `offset` of a cell.
fn word_of<T>(cell: Gc<T>, offset: usize) -> u64 {
    // SAFETY: The callers read fields of the cell that are words.
    unsafe { cell.as_ptr().cast::<u8>().add(offset).cast::<u64>().read_unaligned() }
}

impl SnapshotBuilder<'_> {
    fn cell<T>(&mut self, cell: Gc<T>) -> CellId {
        // SAFETY: Every cell starts with a cell header.
        let header = unsafe { Gc::<CellHeader>::from_non_null(cell.as_non_null().cast()) };
        self.cells.push(header);
        CellId(cell.as_ptr() as u64)
    }

    fn optional_cell<T>(&mut self, cell: Option<Gc<T>>) -> Option<CellId> {
        cell.map(|cell| self.cell(cell))
    }

    fn shape(&mut self, shape: Gc<Shape>) -> ShapeSnapshot {
        ShapeSnapshot {
            shape: self.cell(shape),
            property_count: shape.property_count(),
        }
    }

    fn property_cache(&mut self, cache: &PropertyLookupCache, caller: &Entry) -> PropertyCacheSnapshot {
        let kind = match cache.tier() {
            PropertyLookupCacheTier::Empty => PropertyCacheKind::Empty,
            PropertyLookupCacheTier::Monomorphic => PropertyCacheKind::Monomorphic,
            PropertyLookupCacheTier::Polymorphic => PropertyCacheKind::Polymorphic,
            PropertyLookupCacheTier::Megamorphic => PropertyCacheKind::Megamorphic,
        };
        // NB: The interpreter caches the properties strings, numbers and booleans get from their prototypes as
        //     properties of those prototypes. JIT code speculating on such entries would check for an object and exit
        //     for every primitive, so caches with them are left to the fast paths, which look these up themselves.
        let realm = caller.realm;
        let primitive_prototype_shapes = [
            realm.string_prototype(self.vm).shape(),
            realm.number_prototype(self.vm).shape(),
            realm.boolean_prototype(self.vm).shape(),
        ];
        let mut sees_primitives = false;
        let mut entries = Vec::new();
        for slot in cache.entries() {
            let entry = slot.get();
            if entry
                .shape
                .is_some_and(|shape| primitive_prototype_shapes.contains(&shape))
            {
                sees_primitives = true;
                continue;
            }
            let entry_type = match entry.entry_type {
                // NB: Only the VM's keyed property lookup cache has entries for missing own properties.
                PropertyLookupCacheEntryType::Empty | PropertyLookupCacheEntryType::MissingOwnProperty => continue,
                PropertyLookupCacheEntryType::AddOwnProperty => PropertyCacheEntryType::AddOwnProperty,
                PropertyLookupCacheEntryType::ChangeOwnProperty => PropertyCacheEntryType::ChangeOwnProperty,
                PropertyLookupCacheEntryType::GetOwnProperty => PropertyCacheEntryType::GetOwnProperty,
                PropertyLookupCacheEntryType::ChangePropertyInPrototypeChain => {
                    PropertyCacheEntryType::ChangePropertyInPrototypeChain
                }
                PropertyLookupCacheEntryType::GetPropertyInPrototypeChain => {
                    PropertyCacheEntryType::GetPropertyInPrototypeChain
                }
                PropertyLookupCacheEntryType::GetMissingProperty => PropertyCacheEntryType::GetMissingProperty,
            };
            let prototype_property = prototype_property_object(&entry);
            let accessor_function = entry_accessor_function(slot).map(|function| AccessorFunctionSnapshot {
                function: self.cell(function),
                inline_executable: self
                    .inline_executable_index(Some(Value::from_object(function.upcast::<Object>()).as_cell()), caller),
            });
            entries.push(PropertyCacheEntrySnapshot {
                entry_type,
                property_offset: entry.property_offset,
                shape_dictionary_generation: entry.shape_dictionary_generation,
                shape_is_dictionary: entry.shape.is_some_and(|shape| shape.is_dictionary()),
                // NB: Nothing watches for objects leaving the shape, so no code depends on it staying stable.
                shape_is_stable: false,
                writes_data_property: entry.writes_data_property,
                from_shape: self.optional_cell(entry.from_shape),
                shape: self.optional_cell(entry.shape),
                prototype: self.optional_cell(entry.prototype),
                prototype_chain_validity: self.optional_cell(entry.prototype_chain_validity),
                // NB: Code checks the validity cell, since nothing invalidates code that relied on it.
                prototype_chain_valid: false,
                key: (entry.key != 0).then(|| self.cell(Value(entry.key).as_cell())),
                key_value: entry.key,
                prototype_property: self.optional_cell(prototype_property),
                prototype_property_intrinsic: intrinsic_of(
                    self.vm,
                    prototype_property.map(|object| Value::from_object(object).as_cell()),
                ),
                accessor_function,
                holds_accessor: entry_holds_accessor(slot),
            });
        }
        if sees_primitives {
            entries.clear();
        }
        PropertyCacheSnapshot { kind, entries }
    }

    fn inlined_function(&mut self, function: Gc<EcmascriptFunctionObject>) -> InlinedFunctionSnapshot {
        let realm = function.realm().expect("an ECMAScript function has a realm");
        let executable = function
            .bytecode_executable()
            .expect("an inlined function has an executable");
        InlinedFunctionSnapshot {
            function: self.cell(function),
            formal_parameter_count: function.formal_parameter_count(),
            strict: executable.is_strict_mode,
            // NB: Arrow functions resolve `this` through their environments.
            uses_this: function.uses_this() && function.this_mode() != ThisMode::Lexical,
            global_this: self.cell(realm.global_environment().global_this_value()),
            realm: self.cell(realm),
            shared_data: self.cell(function.shared_data()),
        }
    }

    fn inlined_builtin(&mut self, function: Gc<NativeJavaScriptBackedFunction>) -> InlinedFunctionSnapshot {
        let realm = function.realm();
        let shared_data = function.shared_data();
        InlinedFunctionSnapshot {
            function: self.cell(function),
            formal_parameter_count: shared_data.formal_parameter_count(),
            strict: shared_data.strict(),
            uses_this: true,
            global_this: self.cell(realm.global_environment().global_this_value()),
            realm: self.cell(realm),
            shared_data: self.cell(shared_data),
        }
    }

    /// Whether the target of a call site can be inlined, per the inlining contract in the JIT design.
    fn inlining_candidate(
        &self,
        target: Option<Gc<CellHeader>>,
        caller_realm: Gc<Realm>,
    ) -> Option<Gc<EcmascriptFunctionObject>> {
        let function = ecmascript_function(target)?;
        // NB: can_inline_call() rules out generators, async functions and class constructors.
        if !function.can_inline_call()
            || function.needs_environment_or_this_value_resolution()
            || function.contains_direct_call_to_eval()
            || function.realm() != Some(caller_realm)
        {
            return None;
        }
        let limit = self.vm.jit.options.inline_max_size;
        (instruction_count(&function.inline_call_executable(), limit) <= limit).then_some(function)
    }

    fn inline_executable_index(&mut self, target: Option<Gc<CellHeader>>, caller: &Entry) -> Option<u32> {
        if let Some(builtin) = builtin_inlining_candidate(target, caller.realm) {
            return self.inlined_builtin_index(builtin, caller);
        }
        let function = self.inlining_candidate(target, caller.realm)?;
        self.inlined_function_index(function, caller)
    }

    /// Adds a builtin written in JavaScript as an inlining candidate. Builtins are always inlined, outside the
    /// inlining budget: their calls are what the code would otherwise spend the most on.
    fn inlined_builtin_index(&mut self, builtin: Gc<NativeJavaScriptBackedFunction>, caller: &Entry) -> Option<u32> {
        let function = InlinedFunction::Builtin(builtin);
        if let Some(index) = self.indices.get(&function) {
            return Some(*index);
        }
        if caller.depth + 1 > self.vm.jit.options.inline_depth {
            return None;
        }
        let executable = builtin.shared_data().executable()?;
        let index = u32::try_from(self.entries.len()).expect("the executable count fits in u32");
        self.entries.push(Entry {
            executable,
            function: Some(function),
            realm: builtin.realm(),
            depth: caller.depth + 1,
        });
        self.indices.insert(function, index);
        Some(index)
    }

    /// Adds a function as an inlining candidate of the call sites of `caller`. The snapshot only bounds the candidates
    /// it captures by their size and depth; the graph builder decides which it inlines, within the budget of
    /// `InliningLimits`.
    fn inlined_function_index(&mut self, function: Gc<EcmascriptFunctionObject>, caller: &Entry) -> Option<u32> {
        if let Some(index) = self.indices.get(&InlinedFunction::Ecmascript(function)) {
            return Some(*index);
        }
        let executable = function.bytecode_executable()?;
        if caller.depth + 1 > self.vm.jit.options.inline_depth {
            return None;
        }
        let index = u32::try_from(self.entries.len()).expect("the executable count fits in u32");
        self.entries.push(Entry {
            executable,
            function: Some(InlinedFunction::Ecmascript(function)),
            realm: function.realm().expect("an ECMAScript function has a realm"),
            depth: caller.depth + 1,
        });
        self.indices.insert(InlinedFunction::Ecmascript(function), index);
        Some(index)
    }

    /// Describes how the single callee of a call site forwarded its calls, if the snapshot can describe it.
    fn forwarded_call(&mut self, call: &CallFeedback, caller: &Entry) -> Option<ForwardedCallSnapshot> {
        let target = call.forwarded_target()?;
        let mut snapshot = ForwardedCallSnapshot {
            forwarding: match call.forwarding.get() {
                forwarding if forwarding == CallFeedbackForwarding::Apply as u8 => Forwarding::Apply,
                forwarding if forwarding == CallFeedbackForwarding::Call as u8 => Forwarding::Call,
                forwarding if forwarding == CallFeedbackForwarding::Bound as u8 => Forwarding::Bound,
                forwarding if forwarding == CallFeedbackForwarding::Callback as u8 => Forwarding::Callback,
                _ => return None,
            },
            target: self.cell(target),
            target_intrinsic: intrinsic_of(self.vm, Some(target)),
            argument_count: u32::from(call.forwarded_argument_count.get()),
            inline_executable: None,
            bound_this: 0,
            bound_arguments: [0; 4],
            bound_argument_count: 0,
        };
        if snapshot.forwarding == Forwarding::Bound {
            let bound = cell_as::<BoundFunction>(call.target()?)?;
            let bound_argument_count = bound.bound_arguments_count();
            if bound_argument_count > snapshot.bound_arguments.len() {
                return None;
            }
            // NB: The bound function, which the code checks the callee against and so embeds, keeps its bound values
            //     alive.
            snapshot.bound_this = bound.bound_this().0;
            for index in 0..bound_argument_count {
                snapshot.bound_arguments[index] = bound.bound_argument(index).0;
            }
            snapshot.bound_argument_count = bound_argument_count as u8;
        }
        snapshot.inline_executable = self.inline_executable_index(Some(target), caller);
        Some(snapshot)
    }

    /// Describes the single constructor of a construct site if JIT code can inline its construct: a base constructor
    /// without fields (whose construct only creates `this` before running its body), with a function object of a shape
    /// that has its "prototype" as a data property holding an object.
    fn construct_target(&mut self, target: Option<Gc<CellHeader>>, caller: &Entry) -> Option<ConstructTarget> {
        let function = ecmascript_function(target)?;
        if !function.has_constructor()
            || function.constructor_kind() != ConstructorKind::Base
            || function.has_class_data()
            || function.allocates_function_environment()
            || function.contains_direct_call_to_eval()
            || function.realm() != Some(caller.realm)
        {
            return None;
        }
        // NB: Class constructors cannot be called, only constructed, so their executable is no inline call executable,
        //     but it runs in an inline frame for a construct all the same.
        let executable = function.bytecode_executable()?;
        let limit = MAX_INLINED_CONSTRUCT_INSTRUCTION_COUNT;
        if instruction_count(&executable, limit) > limit {
            return None;
        }
        let function_shape = function.shape();
        if function_shape.is_dictionary() {
            return None;
        }
        let metadata = function_shape.lookup(&self.vm.names.prototype)?;
        let prototype = function.get_direct(metadata.offset);
        if !prototype.is_object() {
            return None;
        }
        let prototype = prototype.as_object();
        // 10.1.13 OrdinaryCreateFromConstructor ( constructor, intrinsicDefaultProto [ , internalSlotsList ] ), https://tc39.es/ecma262/#sec-ordinarycreatefromconstructor
        // NB: The new object gets the shape of an empty object with the prototype, like Object's constructor with a
        //     prototype gives it.
        let empty_shape = caller.realm.intrinsics().empty_object_shape();
        let this_shape = if empty_shape.prototype() == Some(prototype) {
            empty_shape
        } else {
            empty_shape.create_prototype_transition(self.vm, Some(prototype))
        };
        let index = self.inlined_function_index(function, caller)?;
        Some(ConstructTarget {
            executable: index,
            function_shape: self.cell(function_shape),
            prototype_offset: metadata.offset,
            prototype: self.cell(prototype),
            this_shape: self.cell(this_shape),
            reserve: function.shared_data().current_construct_reserve(),
        })
    }

    /// The environment of direct calls of `function`: Some(None) if they need none of their own, and Some(index of
    /// the template) if JIT code can allocate it. Frames of functions without one of their own have the function's
    /// [[Environment]], and those of arrow functions resolve `this` through it.
    fn direct_call_environment(&mut self, function: Gc<EcmascriptFunctionObject>) -> Option<Option<u32>> {
        if !function.function_environment_needed() {
            return Some(None);
        }
        let template = function_environment_template(self.vm, function, |cell| self.cell(cell))?;
        let index = u32::try_from(self.environment_templates.len()).ok()?;
        self.environment_templates.push(template);
        Some(Some(index))
    }

    fn call_feedback(&mut self, call: &CallFeedback, entry: &Entry) -> CallFeedbackSnapshot {
        let target = call.target();
        let flags = call.flags.get();
        let monomorphic = flags & call_feedback_flags::POLYMORPHIC == 0;
        let mut snapshot = CallFeedbackSnapshot {
            target: self.optional_cell(target),
            flags,
            target_intrinsic: intrinsic_of(self.vm, target),
            ..CallFeedbackSnapshot::default()
        };
        if monomorphic {
            snapshot.inline_executable = self.inline_executable_index(target, entry);
            if flags & call_feedback_flags::SAW_CONSTRUCT != 0 {
                snapshot.construct = self.construct_target(target, entry);
            }
            if target.is_some() && flags & call_feedback_flags::FORWARDED_POLYMORPHIC == 0 {
                snapshot.forwarded = self.forwarded_call(call, entry);
            }
        }
        // NB: The environment of a direct call's function environment is the target's, so closures with their own
        //     environments are only called directly if they need no function environment.
        if let Some((function, closures)) = direct_call_target(call)
            && let Some(environment) = self.direct_call_environment(function)
            && !(closures && environment.is_some())
        {
            let executable = function.inline_call_executable();
            let function_fields = function_frame_fields(function);
            let entry = self
                .vm
                .jit
                .entry_table
                .as_ref()
                .expect("the VM has a JIT entry table while the JIT is on")
                .entry_address(executable.head.jit_entry_slot.get());
            snapshot.direct_call = Some(DirectCallTarget {
                function: self.inlined_function(function),
                executable: self.cell(executable),
                entry,
                registers_and_locals_count: executable.registers_and_locals_count(),
                registers_and_locals_and_constants_count: executable
                    .head
                    .registers_and_locals_and_constants_count
                    .get(),
                function_fields,
                environment,
                closures,
            });
        } else if let Some(function) = native_call_target(self.vm, call) {
            let entry = function
                .native_function(self.vm)
                .map_or(0, |pointer| pointer as usize as u64);
            snapshot.native_call = Some(NativeCallTarget {
                function: self.cell(function),
                realm: self.cell(function.realm()),
                entry,
            });
        }
        snapshot
    }

    /// The snapshot of an executable of the snapshot.
    fn executable_snapshot(&mut self, entry: &Entry) -> ExecutableSnapshot {
        let vm = self.vm;
        let Entry { executable, realm, .. } = *entry;
        // NB: Builtins never create arguments objects.
        let arguments_function = match entry.function {
            None => Some(self.compiled_function),
            Some(InlinedFunction::Ecmascript(function)) => Some(function),
            Some(InlinedFunction::Builtin(_)) => None,
        };
        let bytecode = executable.bytecode();

        // NB: Value feedback is only up to date once the values the interpreter recorded since the last update are
        //     folded in.
        // NB: Executables that never left the plain tier have no feedback arrays yet, and an inlined callee may be one.
        let feedback = executable.ensure_feedback();
        feedback.update_value_feedback();
        let call = feedback
            .call()
            .iter()
            .map(|call| self.call_feedback(call, entry))
            .collect();
        let keyed = feedback
            .keyed()
            .iter()
            .map(|keyed| KeyedFeedbackSnapshot { bits: keyed.bits.get() })
            .collect();
        let feedback = FeedbackSnapshot {
            arith: feedback.arith().iter().map(core::cell::Cell::get).collect(),
            value: feedback.value.iter().map(core::cell::Cell::get).collect(),
            call,
            keyed,
        };

        let property_caches = executable
            .property_lookup_caches()
            .iter()
            .map(|cache| self.property_cache(cache, entry))
            .collect();
        let object_shape_caches = executable
            .object_shape_caches()
            .iter()
            .map(|cache| {
                let shape = cache.shape.get()?;
                Some(ObjectShapeCacheSnapshot {
                    shape: self.shape(shape),
                    property_offsets: cache.property_offsets.borrow().clone(),
                })
            })
            .collect();

        let intrinsics = realm.intrinsics();
        ExecutableSnapshot {
            bytecode: bytecode.to_vec(),
            bytecode_address: bytecode.as_ptr() as u64,
            layout: FrameLayout {
                number_of_registers: executable.number_of_registers,
                registers_and_locals_count: executable.registers_and_locals_count(),
                number_of_constants: u32::try_from(executable.constants().len())
                    .expect("the constant count fits in u32"),
                number_of_arguments: executable.number_of_arguments,
            },
            constants: executable.constants().iter().map(|constant| constant.0).collect(),
            exception_handlers: executable
                .exception_handlers
                .iter()
                .map(|handler| ExceptionHandler {
                    start_offset: handler.start_offset,
                    end_offset: handler.end_offset,
                    handler_offset: handler.handler_offset,
                })
                .collect(),
            exit_sites: executable.jit_exit_sites(),
            builtin_exit_sites: if entry.function.is_none() {
                executable
                    .jit_builtin_exit_sites()
                    .into_iter()
                    .map(|(builtin, pc, kind)| (CellId(builtin), pc, kind))
                    .collect()
            } else {
                Vec::new()
            },
            feedback,
            property_caches,
            cell: self.cell(executable),
            function: entry.function.map(|function| match function {
                InlinedFunction::Ecmascript(function) => self.inlined_function(function),
                InlinedFunction::Builtin(function) => self.inlined_builtin(function),
            }),
            function_fields: match entry.function {
                Some(InlinedFunction::Ecmascript(function)) => Some(function_frame_fields(function)),
                // NB: The compiled function's script tells which inlined calls
                //     run in it.
                None => Some(function_frame_fields(self.compiled_function)),
                Some(InlinedFunction::Builtin(_)) => None,
            },
            environment: match entry.function {
                Some(InlinedFunction::Ecmascript(function)) => {
                    function.environment().map(|environment| self.cell(environment))
                }
                _ => None,
            },
            builtin: matches!(entry.function, Some(InlinedFunction::Builtin(_))),
            mapped_arguments_alias_parameters: arguments_function
                .is_some_and(|function| !function.mapped_argument_names().is_empty()),
            new_object_shape: Some(self.shape(intrinsics.new_object_shape())),
            string_prototype: Some(self.cell(realm.string_prototype(vm))),
            number_prototype: Some(self.cell(realm.number_prototype(vm))),
            boolean_prototype: Some(self.cell(realm.boolean_prototype(vm))),
            object_shape_caches,
            closure_templates: if entry.function.is_none() {
                core::mem::take(&mut self.closure_templates)
            } else {
                Vec::new()
            },
            environment_templates: core::mem::take(&mut self.environment_templates),
            lexical_environment_templates: if entry.function.is_none() {
                core::mem::take(&mut self.lexical_environment_templates)
            } else {
                Vec::new()
            },
            // NB: Nothing watches the global variables, so compiled code leaves them to the interpreter's slow paths.
            globals: None,
            // NB: Only the compiled function's own code creates bindings by name.
            identifiers: if entry.function.is_none() {
                executable
                    .identifier_table
                    .iter()
                    .map(|identifier| identifier.raw_identity() as u64)
                    .collect()
            } else {
                Vec::new()
            },
        }
    }
}

/// Captures the snapshot of a compile job for `executable`, whose frames run `function`. `osr_pc` is the loop back
/// edge whose tier-up budget ran out, if a loop triggered the compile.
pub fn capture_snapshot(
    vm: &Vm,
    executable: Gc<Executable>,
    function: Gc<EcmascriptFunctionObject>,
    osr_pc: Option<u32>,
) -> CapturedSnapshot {
    let realm = function.realm().expect("an ECMAScript function has a realm");
    // NB: These allocate (and so may collect garbage), which they do before anything is captured.
    let allocation = allocation_infos(vm, realm);
    let closure_templates = closure_templates(vm, executable, function);
    let closure_samples = closure_templates
        .iter()
        .map(|template| template.as_ref().map(|(sample, _)| *sample))
        .collect::<Vec<_>>();
    let mut builder = SnapshotBuilder {
        vm,
        cells: allocation.cells.clone(),
        entries: vec![Entry {
            executable,
            function: None,
            realm,
            depth: 0,
        }],
        indices: HashMap::new(),
        compiled_function: function,
        closure_templates: Vec::new(),
        environment_templates: Vec::new(),
        lexical_environment_templates: Vec::new(),
    };
    builder.closure_templates = closure_template_snapshots(closure_templates, |sample| builder.cell(sample));
    builder.lexical_environment_templates =
        lexical_environment_templates(vm, executable, realm, |cell| builder.cell(cell));
    // NB: executable_snapshot() appends the inlining candidates it finds.
    let mut executables = Vec::new();
    let mut index = 0;
    while index < builder.entries.len() {
        let entry = builder.entries[index];
        executables.push(builder.executable_snapshot(&entry));
        index += 1;
    }
    // NB: Calls of closures the compiled function creates can be inlined without feedback, as calls of the function
    //     its closure templates are samples of. These come last, so that they take what is left of the inlining budget.
    let root = builder.entries[0];
    for (template_index, sample) in closure_samples.into_iter().enumerate() {
        let Some(sample) = sample else {
            continue;
        };
        // SAFETY: Every cell starts with a cell header.
        let sample_cell = unsafe { Gc::<CellHeader>::from_non_null(sample.as_non_null().cast()) };
        let inline_executable = builder.inline_executable_index(Some(sample_cell), &root);
        if let Some(Some(template)) = executables[0].closure_templates.get_mut(template_index) {
            template.inline_executable = inline_executable;
        }
    }
    while index < builder.entries.len() {
        let entry = builder.entries[index];
        executables.push(builder.executable_snapshot(&entry));
        index += 1;
    }
    let runtime = super::runtime_info::runtime_info(vm, realm, allocation);
    // NB: Code that embeds the prototypes of the runtime info lists them as embedded cells.
    builder.cell(realm.array_prototype());
    builder.cell(realm.object_prototype());
    let options = &vm.jit.options;
    let snapshot = Snapshot {
        executables,
        runtime,
        options: CompileOptions {
            dump_ir: options.dump_ir,
            dump_passes: options.dump_passes,
            dump_asm: options.dump_asm,
            verify_ir: options.verify_ir,
            osr_pc,
            inlining: InliningLimits {
                always_inlined_instructions: ALWAYS_INLINED_INSTRUCTION_COUNT,
                max_instructions: options.inline_max_size,
                budget_instructions: options.inline_budget,
                max_construct_instructions: MAX_INLINED_CONSTRUCT_INSTRUCTION_COUNT,
                max_depth: options.inline_depth,
            },
            coverage: options.coverage.is_some(),
            stress: StressOptions {
                exit_countdown: vm.jit.stress_exit_countdown_address(),
                osr_at_every_loop: options.stress_osr,
                few_registers: options.stress_registers,
            },
        },
    };
    CapturedSnapshot {
        snapshot: Box::new(snapshot),
        cells: builder.cells,
        snapshot_executables: builder
            .entries
            .iter()
            .map(|entry| SnapshotExecutable {
                executable: entry.executable,
                function: entry.function,
            })
            .collect(),
    }
}
