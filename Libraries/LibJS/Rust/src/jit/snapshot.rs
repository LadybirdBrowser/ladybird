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

use libjs_jit::bytecode::{EnvironmentCoordinate, ExceptionHandler, FrameLayout, Instruction, OpCode, decode_all};
use libjs_jit::code::ExitKind;
use libjs_jit::snapshot::{
    AccessorFunctionSnapshot, CallFeedbackSnapshot, CellId, ClosureTemplateSnapshot, CompileOptions, ConstructTarget,
    DirectCallTarget, DirectGetterFields, EnvironmentTemplateSnapshot, ExecutableSnapshot, FeedbackSnapshot,
    ForwardedCallSnapshot, Forwarding, FunctionFrameFields, GlobalCacheSnapshot, GlobalValueSnapshot, GlobalsSnapshot,
    InlinedFunctionSnapshot, InliningLimits, Intrinsic, KeyedFeedbackSnapshot, LexicalEnvironmentTemplateSnapshot,
    NativeCallTarget, ObjectShapeCacheSnapshot, OrdinaryHasInstanceSnapshot, PropertyCacheEntrySnapshot,
    PropertyCacheEntryType, PropertyCacheKind, PropertyCacheSnapshot, ShapeSnapshot, Snapshot, StressOptions,
    TypedArraySnapshot,
};

use super::allocation::{
    allocation_infos, closure_template_snapshots, closure_templates, function_environment_template,
    lexical_environment_templates,
};
use super::code::{CapturedBindingAccesses, InlinedFunction, SnapshotExecutable, speculated_cache_size};
use super::runtime_info::RAW_NATIVE_FUNCTIONS_RETURN_IN_REGISTERS;
use crate::bytecode::executable::{Executable, PropertyLookupCacheEntryData, PropertyLookupCacheTier};
use crate::bytecode::feedback::{CallFeedback, CallFeedbackForwarding, call_feedback_flags};
use crate::bytecode::instruction::instruction_length_from_bytes;
use crate::gc::class::Extends;
use crate::gc::defer_gc::{DeferGc, Deferred};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::environment::DeclarativeEnvironment;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::function_object::{
    DirectGetterFunction, EcmascriptFunctionObject, FunctionObject, RawNativeFunction,
};
use crate::layout::object::{Object, TYPED_ARRAY_CACHED_DATA_OFFSET_INVALID, TypedArrayBase};
use crate::layout::property_lookup_cache::{GlobalVariableCache, PropertyLookupCache, PropertyLookupCacheEntryType};
use crate::layout::value::Value;
use crate::layout_forward::RawNativeFunctionPointer;
use crate::runtime::array_constructor::ArrayConstructor;
use crate::runtime::array_prototype;
use crate::runtime::boolean_constructor::BooleanConstructor;
use crate::runtime::bound_function::BoundFunction;
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;
use crate::runtime::function_prototype::{self, FunctionPrototype};
use crate::runtime::map_prototype;
use crate::runtime::math_object;
use crate::runtime::native_javascript_backed_function::NativeJavaScriptBackedFunction;
use crate::runtime::object_constructor::ObjectConstructor;
use crate::runtime::object_prototype;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::realm_watchpoints;
use crate::runtime::shape::Shape;
use crate::runtime::shared_function_instance_data::ClosureCount;
use crate::runtime::shared_function_instance_data::ConstructorKind;
use crate::runtime::shared_function_instance_data::FunctionKind;
use crate::runtime::shared_function_instance_data::ThisMode;
use crate::runtime::string_constructor::StringConstructor;
use crate::runtime::string_prototype;
use crate::runtime::typed_array_prototype;

/// Candidates with at most this many bytecode instructions are always inlined, without counting against the inlining
/// budget.
const ALWAYS_INLINED_INSTRUCTION_COUNT: u32 = 10;

/// Constructors of at most this many instructions are inlined into their constructs (see
/// `InliningLimits::max_construct_instructions`).
const MAX_INLINED_CONSTRUCT_INSTRUCTION_COUNT: u32 = 50;

/// A snapshot with the cells it refers to, which must stay alive (and in place) until the job's code is installed or
/// the job is abandoned.
pub struct CapturedSnapshot<'d> {
    pub snapshot: Box<Snapshot>,
    pub cells: Deferred<'d, Vec<Gc<CellHeader>>>,
    pub snapshot_executables: Deferred<'d, Vec<SnapshotExecutable>>,
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
    _defer_gc: &'vm DeferGc<'vm>,
    cells: Vec<Gc<CellHeader>>,
    entries: Vec<Entry>,
    indices: HashMap<InlinedFunction, u32>,
    /// The function the compiled executable runs, or `None` for the top-level code of a script or of a module.
    compiled_function: Option<Gc<EcmascriptFunctionObject>>,
    /// The words the compiled executable's frames start with (see `FunctionFrameFields`).
    compiled_function_fields: FunctionFrameFields,
    /// The closure templates of the compiled executable, until its snapshot takes them. Only the compiled function
    /// allocates closures itself.
    closure_templates: Vec<Option<ClosureTemplateSnapshot>>,
    /// The lexical environment templates of the compiled executable, until its snapshot takes them.
    lexical_environment_templates: Vec<Option<LexicalEnvironmentTemplateSnapshot>>,
    /// The environment templates of the direct calls of the executable whose snapshot is being made.
    environment_templates: Vec<EnvironmentTemplateSnapshot>,
    /// The bound arguments of the call feedback of the executable whose snapshot is being made.
    bound_arguments: Vec<u64>,
    /// For the constructors among the inlining candidates, by index: the shape of the `this` value their inlined
    /// constructs create, whose entries their megamorphic caches describe.
    construct_this_shapes: HashMap<u32, Gc<Shape>>,
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
    as_ecmascript_function_object(cell?.downcast::<Object>()?)
}

/// The target of a call site if it is a builtin written in JavaScript that JIT code can inline: a normal function
/// without a function environment of the caller's realm, which already ran.
fn builtin_inlining_candidate(
    target: Option<Gc<CellHeader>>,
    caller_realm: Gc<Realm>,
) -> Option<Gc<NativeJavaScriptBackedFunction>> {
    let builtin = target?.downcast::<NativeJavaScriptBackedFunction>()?;
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
    if cell.downcast::<StringConstructor>().is_some() {
        return Some(Intrinsic::StringConstructor);
    }
    if cell.downcast::<ArrayConstructor>().is_some() {
        return Some(Intrinsic::ArrayConstructor);
    }
    if cell.downcast::<ObjectConstructor>().is_some() {
        return Some(Intrinsic::ObjectConstructor);
    }
    if cell.downcast::<BooleanConstructor>().is_some() {
        return Some(Intrinsic::BooleanConstructor);
    }
    let function = cell.downcast::<RawNativeFunction>()?;
    match function.builtin() {
        Some(Builtin::StringFromCharCode) => return Some(Intrinsic::StringFromCharCode),
        Some(Builtin::RegExpPrototypeExec) => return Some(Intrinsic::RegExpPrototypeExec),
        _ => {}
    }
    let address = function.native_function(vm)? as usize;
    NATIVE_INTRINSICS
        .iter()
        .find(|(native_function, _)| native_function.is_some_and(|native_function| native_function as usize == address))
        .map(|(_, intrinsic)| *intrinsic)
}

/// The intrinsics the compiler knows by their native functions (of any realm).
static NATIVE_INTRINSICS: [(&RawNativeFunctionPointer, Intrinsic); 24] = [
    (&function_prototype::APPLY_FUNCTION, Intrinsic::FunctionPrototypeApply),
    (&function_prototype::CALL_FUNCTION, Intrinsic::FunctionPrototypeCall),
    (&array_prototype::PUSH_FUNCTION, Intrinsic::ArrayPrototypePush),
    (&array_prototype::SLICE_FUNCTION, Intrinsic::ArrayPrototypeSlice),
    (&array_prototype::SPLICE_FUNCTION, Intrinsic::ArrayPrototypeSplice),
    (
        &object_prototype::HAS_OWN_PROPERTY_FUNCTION,
        Intrinsic::ObjectPrototypeHasOwnProperty,
    ),
    (
        &typed_array_prototype::LENGTH_GETTER,
        Intrinsic::TypedArrayPrototypeLength,
    ),
    (&math_object::MAX_FUNCTION, Intrinsic::MathMax),
    (&math_object::MIN_FUNCTION, Intrinsic::MathMin),
    (&math_object::IMUL_FUNCTION, Intrinsic::MathImul),
    (&string_prototype::INDEX_OF_FUNCTION, Intrinsic::StringPrototypeIndexOf),
    (
        &string_prototype::TO_LOWERCASE_FUNCTION,
        Intrinsic::StringPrototypeToLowerCase,
    ),
    (
        &string_prototype::TO_UPPERCASE_FUNCTION,
        Intrinsic::StringPrototypeToUpperCase,
    ),
    (
        &string_prototype::LAST_INDEX_OF_FUNCTION,
        Intrinsic::StringPrototypeLastIndexOf,
    ),
    (&string_prototype::INCLUDES_FUNCTION, Intrinsic::StringPrototypeIncludes),
    (
        &string_prototype::STARTS_WITH_FUNCTION,
        Intrinsic::StringPrototypeStartsWith,
    ),
    (
        &string_prototype::ENDS_WITH_FUNCTION,
        Intrinsic::StringPrototypeEndsWith,
    ),
    (&string_prototype::SLICE_FUNCTION, Intrinsic::StringPrototypeSlice),
    (
        &string_prototype::SUBSTRING_FUNCTION,
        Intrinsic::StringPrototypeSubstring,
    ),
    (&string_prototype::TRIM_FUNCTION, Intrinsic::StringPrototypeTrim),
    (
        &string_prototype::TRIM_START_FUNCTION,
        Intrinsic::StringPrototypeTrimStart,
    ),
    (&string_prototype::TRIM_END_FUNCTION, Intrinsic::StringPrototypeTrimEnd),
    (&map_prototype::GET_FUNCTION, Intrinsic::MapPrototypeGet),
    (&map_prototype::HAS_FUNCTION, Intrinsic::MapPrototypeHas),
];

/// The builtin `%ArrayIteratorPrototype%.next` of the realm, if `%ArrayIteratorPrototype%` holds it.
fn array_iterator_next(vm: &Vm, realm: Gc<Realm>) -> Option<Gc<FunctionObject>> {
    let next = realm
        .array_iterator_prototype()
        .get_without_side_effects(vm, &vm.names.next);
    if !next.is_function() {
        return None;
    }
    let next = next.as_function();
    (next.as_native_function().is_some() && next.is_array_prototype_next_builtin()).then_some(next)
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

/// Whether the own property an entry reads or writes is writable in the shape of the object, which makes it a data
/// property for as long as the object keeps the shape (see `Object::storage_set()`). Loads of the properties of
/// prototypes always check, as they are guarded by no check of the object they load from.
fn property_holds_data(entry: &PropertyLookupCacheEntryData) -> bool {
    if !matches!(
        entry.entry_type,
        PropertyLookupCacheEntryType::GetOwnProperty | PropertyLookupCacheEntryType::ChangeOwnProperty
    ) {
        return false;
    }
    let Some(shape) = entry.shape else {
        return false;
    };
    let mut writable = false;
    shape.for_each_property_in_insertion_order(|_, metadata| {
        if metadata.offset != entry.property_offset {
            return core::ops::ControlFlow::Continue(());
        }
        writable = metadata.attributes.is_writable();
        core::ops::ControlFlow::Break(())
    });
    writable
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

/// The function of the accessor an entry calls: for a GetPropertyInPrototypeChain
/// entry, the getter of the accessor its prototype holds in the property, for a ChangePropertyInPrototypeChain entry
/// its setter, and for a GetOwnProperty entry the getter the interpreter last called.
fn entry_accessor_function(entry: &PropertyLookupCacheEntryData) -> Option<Gc<FunctionObject>> {
    let (accessor, setter) = match entry.entry_type {
        PropertyLookupCacheEntryType::GetOwnProperty if entry.key == 0 => return entry.getter,
        entry_type => {
            let value = prototype_property_value(entry)?;
            if !value.is_accessor() {
                return None;
            }
            (
                value.as_accessor(),
                entry_type == PropertyLookupCacheEntryType::ChangePropertyInPrototypeChain,
            )
        }
    };
    if setter { accessor.setter() } else { accessor.getter() }
}

/// Whether the property of an entry held an accessor: the accessor the interpreter last called the getter of for a
/// GetOwnProperty entry, or the value its prototype holds in the property for entries of a prototype.
fn entry_holds_accessor(entry: &PropertyLookupCacheEntryData) -> bool {
    match entry.entry_type {
        PropertyLookupCacheEntryType::GetOwnProperty if entry.key == 0 => entry.getter.is_some(),
        _ => prototype_property_value(entry).is_some_and(|value| value.is_accessor()),
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
    let function = feedback.target()?.downcast::<RawNativeFunction>()?;
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

/// The words of the `[[ScriptOrModule]]` of a frame, like `function_frame_fields()` has them of a function.
fn script_or_module_words(frame: &ExecutionContext) -> [u64; 2] {
    let address = core::ptr::from_ref(&frame.script_or_module).cast::<u64>();
    // SAFETY: The field is two words.
    unsafe { [address.read_unaligned(), address.add(1).read_unaligned()] }
}

/// The words at `offset` of a cell.
fn word_of<T>(cell: Gc<T>, offset: usize) -> u64 {
    // SAFETY: The callers read fields of the cell that are words.
    unsafe { cell.as_ptr().cast::<u8>().add(offset).cast::<u64>().read_unaligned() }
}

impl SnapshotBuilder<'_> {
    fn cell<T: Extends<CellHeader>>(&mut self, cell: Gc<T>) -> CellId {
        let header = cell.upcast::<CellHeader>();
        self.cells.push(header);
        CellId(cell.as_ptr() as u64)
    }

    fn optional_cell<T: Extends<CellHeader>>(&mut self, cell: Option<Gc<T>>) -> Option<CellId> {
        cell.map(|cell| self.cell(cell))
    }

    fn shape(&mut self, shape: Gc<Shape>) -> ShapeSnapshot {
        ShapeSnapshot {
            shape: self.cell(shape),
            property_count: shape.property_count(),
        }
    }

    /// What property lookup cache `cache` of `caller` knows. Of a megamorphic cache, that is the entry for
    /// `known_shape` if it has one: JIT code only uses megamorphic entries for objects whose shape it knows.
    fn property_cache(
        &mut self,
        cache: &PropertyLookupCache,
        caller: &Entry,
        known_shape: Option<Gc<Shape>>,
    ) -> PropertyCacheSnapshot {
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
        let known_shape_slot = known_shape.and_then(|shape| cache.megamorphic_entry_for_shape(shape, 0));
        let slots = match known_shape_slot {
            Some(slot) => core::slice::from_ref(slot),
            None => cache.entries(),
        };
        for slot in slots {
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
            let cached_value_offset = prototype_property_value(&entry)
                .filter(|value| value.is_accessor() && value.as_accessor().cache_on_receiver())
                .and_then(|value| value.as_accessor().cached_value_key())
                .and_then(|key| entry.shape?.lookup(&PropertyKey::from_symbol(key)))
                .map(|metadata| metadata.offset);
            let accessor_function = entry_accessor_function(&entry).map(|function| {
                let cell = self.cell(function);
                AccessorFunctionSnapshot {
                    cached_value_offset,
                    function: cell,
                    intrinsic: intrinsic_of(self.vm, Some(Value::from_object(function.upcast::<Object>()).as_cell())),
                    inline_executable: self.inline_executable_index(
                        Some(Value::from_object(function.upcast::<Object>()).as_cell()),
                        caller,
                    ),
                    native: self.native_accessor_target(function, cell, entry.getter == Some(function)),
                }
            });
            entries.push(PropertyCacheEntrySnapshot {
                entry_type,
                property_offset: entry.property_offset,
                shape_dictionary_generation: entry.shape_dictionary_generation,
                shape_is_dictionary: entry.shape.is_some_and(|shape| shape.is_dictionary()),
                shape_is_stable: entry.shape.is_some_and(|shape| shape.is_stable()),
                writes_data_property: entry.writes_data_property,
                from_shape: self.optional_cell(entry.from_shape),
                shape: self.optional_cell(entry.shape),
                prototype: self.optional_cell(entry.prototype),
                prototype_chain_validity: self.optional_cell(entry.prototype_chain_validity),
                prototype_chain_valid: entry
                    .prototype_chain_validity
                    .is_some_and(|validity| validity.is_valid()),
                key: (entry.key != 0).then(|| self.cell(Value::from_bits(entry.key).as_cell())),
                key_value: entry.key,
                prototype_property: self.optional_cell(prototype_property),
                prototype_property_intrinsic: intrinsic_of(
                    self.vm,
                    prototype_property.map(|object| Value::from_object(object).as_cell()),
                ),
                prototype_property_inline_executable: prototype_property.and_then(|object| {
                    self.inline_executable_index(Some(Value::from_object(object).as_cell()), caller)
                }),
                prototype_property_direct_call: prototype_property
                    .and_then(|object| self.prototype_method_direct_call(object)),
                // NB: Watching a slot allocates, which collects no garbage while the snapshot is taken. Slots
                //     replaced while watched are checked instead, as invalidated code learns nothing, while the
                //     check learns from its exit.
                prototype_property_watchpoint: prototype_property
                    .and(entry.prototype)
                    .filter(|holder| !holder.shape().was_slot_replaced(entry.property_offset))
                    .and_then(|holder| {
                        holder
                            .shape()
                            .watch_slot_for_replacements(self.vm, entry.property_offset)
                    })
                    .map(|set| self.cell(set)),
                accessor_function,
                property_holds_data: property_holds_data(&entry),
                holds_accessor: entry_holds_accessor(&entry),
            });
        }
        if sees_primitives {
            entries.clear();
        }
        PropertyCacheSnapshot { kind, entries }
    }

    /// What the global variable caches of `executable`, whose frames run in `realm`, know about the global variables
    /// of the realm, as far as it still holds.
    fn globals(&mut self, executable: Gc<Executable>, realm: Gc<Realm>) -> GlobalsSnapshot {
        let object = realm.global_object();
        let environment = realm.global_declarative_environment();
        let environment_serial = environment.environment_serial_number();
        let shape = object.shape();
        let mut caches = Vec::with_capacity(executable.global_variable_caches().len());
        for cache in executable.global_variable_caches() {
            caches.push(self.global_cache(cache, realm, object, environment, environment_serial, shape));
        }
        GlobalsSnapshot {
            object: self.cell(object),
            declarative_environment: self.cell(environment),
            environment_serial,
            caches,
        }
    }

    /// What a global variable cache found, like `GetGlobal` and `SetGlobal` use it, if it still applies.
    fn global_cache(
        &mut self,
        cache: &GlobalVariableCache,
        realm: Gc<Realm>,
        object: Gc<Object>,
        environment: Gc<DeclarativeEnvironment>,
        environment_serial: u64,
        shape: Gc<Shape>,
    ) -> Option<GlobalCacheSnapshot> {
        if cache.environment_serial_number.get() != environment_serial {
            return None;
        }
        if let Some(entry) = cache.first_entry()
            && entry.shape == Some(shape)
            && (!shape.is_dictionary() || shape.dictionary_generation() == entry.shape_dictionary_generation)
        {
            let value = object.get_direct(entry.property_offset);
            if value.is_accessor() {
                return None;
            }
            return Some(GlobalCacheSnapshot::Property {
                shape: self.cell(shape),
                dictionary_generation: shape.is_dictionary().then(|| shape.dictionary_generation()),
                offset: entry.property_offset,
                writes_data_property: entry.entry_type == PropertyLookupCacheEntryType::ChangeOwnProperty
                    && entry.writes_data_property,
                assigned: super::dependencies::global_object_slot_may_have_been_assigned(
                    self.vm,
                    &object,
                    entry.property_offset,
                ),
                value: self.global_value(value, realm),
            });
        }
        if !cache.has_environment_binding_index.get() {
            return None;
        }
        let index = cache.environment_binding_index.get();
        let value = environment.get_initialized_binding_value_direct(index as usize);
        Some(GlobalCacheSnapshot::Binding {
            index,
            mutable: environment.binding_is_mutable(index as usize),
            assigned: environment.binding_may_have_been_assigned(index as usize),
            value: (!value.is_empty()).then(|| self.global_value(value, realm)),
        })
    }

    fn global_value(&mut self, value: Value, realm: Gc<Realm>) -> GlobalValueSnapshot {
        let cell = value.is_cell().then(|| value.as_cell());
        GlobalValueSnapshot {
            bits: value.0,
            cell: cell.map(|cell| self.cell(cell)),
            intrinsic: intrinsic_of(self.vm, cell),
            has_instance: self.ordinary_has_instance(value, realm),
            typed_array: value
                .is_object()
                .then(|| value.as_object().downcast::<TypedArrayBase>())
                .flatten()
                .and_then(|array| {
                    use crate::runtime::typed_array::Kind;
                    use libjs_jit::ir::TypedArrayElement;
                    let element = match array.kind() {
                        Kind::Uint8Array => TypedArrayElement::Uint8,
                        Kind::Uint8ClampedArray => TypedArrayElement::Uint8Clamped,
                        Kind::Int8Array => TypedArrayElement::Int8,
                        Kind::Uint16Array => TypedArrayElement::Uint16,
                        Kind::Int16Array => TypedArrayElement::Int16,
                        Kind::Uint32Array => TypedArrayElement::Uint32,
                        Kind::Int32Array => TypedArrayElement::Int32,
                        Kind::Float32Array => TypedArrayElement::Float32,
                        Kind::Float64Array => TypedArrayElement::Float64,
                        _ => return None,
                    };
                    Some(TypedArraySnapshot {
                        element,
                        fixed_length: (array.cached_data_offset() != TYPED_ARRAY_CACHED_DATA_OFFSET_INVALID)
                            .then(|| array.array_length.length.get()),
                    })
                }),
        }
    }

    /// How `instanceof` with `value` on its right-hand side runs, if `value` is a function that inherits the
    /// `@@hasInstance` of `%Function.prototype%` of `realm`, which is neither writable nor configurable: its shape, which
    /// tells that while the function keeps it, and where its `prototype` property is.
    fn ordinary_has_instance(&mut self, value: Value, realm: Gc<Realm>) -> Option<OrdinaryHasInstanceSnapshot> {
        if !value.is_function() || value.as_function().downcast::<BoundFunction>().is_some() {
            return None;
        }
        let function = value.as_object();
        let shape = function.shape();
        let has_instance = PropertyKey::from(self.vm.well_known_symbols().has_instance);
        if !function.has_ordinary_get_own_property_for(self.vm, &has_instance)
            || shape.prototype() != Some(realm.function_prototype())
            || shape.lookup(&has_instance).is_some()
        {
            return None;
        }
        // NB: ECMAScript functions create their prototype property lazily, but one in their shape is the one they have.
        let prototype = shape.lookup(&self.vm.names.prototype)?;
        if function.get_direct(prototype.offset).is_accessor() {
            return None;
        }
        Some(OrdinaryHasInstanceSnapshot {
            shape: self.cell(shape),
            dictionary_generation: shape.is_dictionary().then(|| shape.dictionary_generation()),
            prototype_offset: prototype.offset,
        })
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
        let forwarding = match call.forwarding.get() {
            forwarding if forwarding == CallFeedbackForwarding::Apply as u8 => Forwarding::Apply,
            forwarding if forwarding == CallFeedbackForwarding::Call as u8 => Forwarding::Call,
            forwarding if forwarding == CallFeedbackForwarding::Bound as u8 => Forwarding::Bound,
            forwarding if forwarding == CallFeedbackForwarding::Callback as u8 => Forwarding::Callback,
            _ => return None,
        };
        let bound = match forwarding {
            Forwarding::Bound => Some(call.target()?.downcast::<BoundFunction>()?),
            _ => None,
        };
        let mut snapshot = ForwardedCallSnapshot {
            forwarding,
            target: self.cell(target),
            target_intrinsic: intrinsic_of(self.vm, Some(target)),
            argument_count: u32::from(call.forwarded_argument_count.get()),
            inline_executable: match bound {
                Some(bound) if bound.bound_arguments().len() > ForwardedCallSnapshot::MAX_INLINED_BOUND_ARGUMENTS => {
                    None
                }
                _ => self.inline_executable_index(Some(target), caller),
            },
            bound_this: 0,
            bound_arguments_start: 0,
            bound_argument_count: 0,
        };
        // NB: Only calls of inlining candidates use the bound values.
        if let Some(bound) = bound
            && snapshot.inline_executable.is_some()
        {
            // NB: The bound function, which the code checks the callee against and so embeds, keeps its bound values
            //     alive.
            let bound_arguments = bound.bound_arguments();
            snapshot.bound_this = bound.bound_this().0;
            snapshot.bound_arguments_start = u32::try_from(self.bound_arguments.len()).ok()?;
            snapshot.bound_argument_count = u32::try_from(bound_arguments.len()).ok()?;
            self.bound_arguments.extend(bound_arguments.iter().map(|value| value.0));
        }
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
        self.construct_this_shapes.insert(index, this_shape);
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

    fn direct_call_target_snapshot(
        &mut self,
        function: Gc<EcmascriptFunctionObject>,
        environment: Option<u32>,
        closures: bool,
    ) -> DirectCallTarget {
        let executable = function.inline_call_executable();
        let entry = self
            .vm
            .jit
            .entry_table
            .as_ref()
            .expect("the VM has a JIT entry table while the JIT is on")
            .entry_address(executable.head.jit_entry_slot.get());
        DirectCallTarget {
            function: self.inlined_function(function),
            executable: self.cell(executable),
            entry,
            registers_and_locals_count: executable.registers_and_locals_count(),
            registers_and_locals_and_constants_count: executable.head.registers_and_locals_and_constants_count.get(),
            function_fields: function_frame_fields(function),
            environment,
            closures,
        }
    }

    /// The target through which JIT code may call `object`, a method a prototype held, directly, if it can and its
    /// calls need no function environment (whose template would belong to the executable of the call).
    fn prototype_method_direct_call(&mut self, object: Gc<Object>) -> Option<DirectCallTarget> {
        let function = as_ecmascript_function_object(object)?;
        if !function.can_inline_call() || function.function_environment_needed() {
            return None;
        }
        Some(self.direct_call_target_snapshot(function, None, false))
    }

    fn call_feedback(&mut self, call: &CallFeedback, entry: &Entry) -> CallFeedbackSnapshot {
        let target = call.target();
        let flags = call.flags.get();
        let monomorphic = flags & call_feedback_flags::POLYMORPHIC == 0;
        let mut snapshot = CallFeedbackSnapshot {
            target: self.optional_cell(target),
            flags,
            target_intrinsic: intrinsic_of(self.vm, target),
            target_builtin: target
                .and_then(|target| target.downcast::<RawNativeFunction>()?.builtin())
                .map(|builtin| builtin as u8),
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
            snapshot.direct_call = Some(self.direct_call_target_snapshot(function, environment, closures));
        } else if let Some(function) = native_call_target(self.vm, call) {
            let entry = function
                .native_function(self.vm)
                .map_or(0, |pointer| pointer as usize as u64);
            snapshot.native_call = Some(NativeCallTarget {
                function: self.cell(function),
                realm: self.cell(function.realm()),
                entry,
                direct_getter: None,
            });
        }
        snapshot
    }

    /// The target through which JIT code may call an accessor's function, whose cell is `function_cell`, directly,
    /// if it is a raw native function. Like the interpreter, JIT code reads the value of a direct getter without
    /// calling it only through a cache entry on which the getter ran (`direct_getter_validated`).
    fn native_accessor_target(
        &mut self,
        function: Gc<FunctionObject>,
        function_cell: CellId,
        direct_getter_validated: bool,
    ) -> Option<NativeCallTarget> {
        if !RAW_NATIVE_FUNCTIONS_RETURN_IN_REGISTERS {
            return None;
        }
        let native_function = function.downcast::<RawNativeFunction>()?;
        let entry = native_function.native_function(self.vm)? as usize as u64;
        let direct_getter = function
            .downcast::<DirectGetterFunction>()
            .filter(|_| direct_getter_validated)
            .map(|getter| {
                let bytes = |word_offset: u32| word_offset * size_of::<u64>() as u32;
                DirectGetterFields {
                    wrapper_implementation: bytes(getter.wrapper_implementation_word_offset.get()),
                    implementation_value: bytes(getter.implementation_value_word_offset.get()),
                    main_world_wrapper: bytes(getter.main_world_wrapper_word_offset.get()),
                    weak_impl_value: bytes(getter.weak_impl_value_word_offset.get()),
                }
            });
        Some(NativeCallTarget {
            function: function_cell,
            realm: self.cell(native_function.realm()),
            entry,
            direct_getter,
        })
    }

    /// The snapshot of an executable of the snapshot.
    fn executable_snapshot(&mut self, entry: &Entry) -> ExecutableSnapshot {
        let vm = self.vm;
        let Entry { executable, realm, .. } = *entry;
        // NB: Builtins never create arguments objects.
        let arguments_function = match entry.function {
            None => self.compiled_function,
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
            bound_arguments: core::mem::take(&mut self.bound_arguments),
        };

        let construct_this_shape = entry
            .function
            .and_then(|function| self.indices.get(&function))
            .and_then(|index| self.construct_this_shapes.get(index))
            .copied();
        let property_caches = executable
            .property_lookup_caches()
            .iter()
            .map(|cache| self.property_cache(cache, entry, construct_this_shape))
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
            exit_sites: executable.jit_exit_sites_for_recompile(),
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
                None => Some(self.compiled_function_fields),
                Some(InlinedFunction::Builtin(_)) => None,
            },
            environment: match entry.function {
                Some(InlinedFunction::Ecmascript(function)) => {
                    function.environment().map(|environment| self.cell(environment))
                }
                _ => None,
            },
            captured_bindings: arguments_function
                .map(|function| self.captured_bindings(function, executable, realm))
                .unwrap_or_default(),
            mutable_bindings: arguments_function
                .map(|function| mutable_bindings(function, executable))
                .unwrap_or_default(),
            builtin: matches!(entry.function, Some(InlinedFunction::Builtin(_))),
            mapped_arguments_alias_parameters: arguments_function
                .is_some_and(|function| !function.mapped_argument_names().is_empty()),
            supports_legacy_arguments: arguments_function
                .is_some_and(|function| function.supports_legacy_caller_or_arguments()),
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
            // NB: Builtins written in JavaScript never access global variables by name.
            globals: (!matches!(entry.function, Some(InlinedFunction::Builtin(_))))
                .then(|| self.globals(executable, realm)),
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

    /// Object values of captured bindings of functions whose lexical environment is always their closure's
    /// environment, while the function has no other closure, so that every frame running the code sees these
    /// bindings. Their loads are guarded, so a reassignment or a closure created later can change the value safely.
    /// A binding whose guard failed at any of its loads changes, so none of its loads speculates on it again.
    fn captured_bindings(
        &mut self,
        function: Gc<EcmascriptFunctionObject>,
        executable: Gc<Executable>,
        realm: Gc<Realm>,
    ) -> Vec<(EnvironmentCoordinate, GlobalValueSnapshot)> {
        if function.allocates_function_environment() || function.shared_data().closure_count() != ClosureCount::One {
            return Vec::new();
        }
        let loads = &executable
            .jit_state()
            .captured_binding_accesses
            .get_or_init(|| captured_binding_accesses(&executable))
            .loads;
        let exit_sites = executable.jit_exit_sites();
        let changed: Vec<EnvironmentCoordinate> = loads
            .iter()
            .filter(|&&(_, pc)| exit_sites.contains(&(pc, ExitKind::UnexpectedValue)))
            .map(|&(coordinate, _)| coordinate)
            .collect();
        let mut bindings: Vec<(EnvironmentCoordinate, GlobalValueSnapshot)> = Vec::new();
        for &(coordinate, _) in loads {
            if changed.contains(&coordinate) || bindings.iter().any(|(captured, _)| *captured == coordinate) {
                continue;
            }
            let mut environment = function.environment();
            for _ in 0..coordinate.hops {
                environment = environment.and_then(|environment| environment.outer_environment());
                if environment.is_none() {
                    break;
                }
            }
            let Some(environment) = environment else {
                continue;
            };
            let Some(environment) = environment.as_declarative_environment() else {
                continue;
            };
            if coordinate.index as usize >= environment.binding_values.size() {
                continue;
            }
            let value = environment.get_initialized_binding_value_direct(coordinate.index as usize);
            if value.is_object() {
                bindings.push((coordinate, self.global_value(value, realm)));
            }
        }
        bindings
    }
}

/// The bindings of the environments of `function`'s closure that `executable` stores to and that are mutable in every
/// instance of their environment: declared mutable, and never deleted. Its stores to them need no check of that. Like
/// captured bindings, these are only found while the function has no other closure.
fn mutable_bindings(function: Gc<EcmascriptFunctionObject>, executable: Gc<Executable>) -> Vec<EnvironmentCoordinate> {
    if function.allocates_function_environment() || function.shared_data().closure_count() != ClosureCount::One {
        return Vec::new();
    }
    let stores = &executable
        .jit_state()
        .captured_binding_accesses
        .get_or_init(|| captured_binding_accesses(&executable))
        .stores;
    let mut bindings = Vec::new();
    for &coordinate in stores {
        if bindings.contains(&coordinate) {
            continue;
        }
        let mut environment = function.environment();
        for _ in 0..coordinate.hops {
            environment = environment.and_then(|environment| environment.outer_environment());
        }
        let Some(environment) = environment else {
            continue;
        };
        let Some(environment) = environment.as_declarative_environment() else {
            continue;
        };
        if environment.binding_is_declared_mutable(coordinate.index as usize) {
            bindings.push(coordinate);
        }
    }
    bindings
}

/// The loads and stores of bindings of environments in `executable`, or none if its code may change its lexical
/// environment, so that they are not the environments of its closure.
fn captured_binding_accesses(executable: &Executable) -> CapturedBindingAccesses {
    let Ok(instructions) = decode_all(executable.bytecode()) else {
        return CapturedBindingAccesses::default();
    };
    let changes_environment = instructions.iter().any(|decoded| {
        matches!(
            decoded.instruction.opcode(),
            OpCode::SetLexicalEnvironment
                | OpCode::CreateLexicalEnvironment
                | OpCode::CreateVariableEnvironment
                | OpCode::EnterObjectEnvironment
                | OpCode::NewClass
                | OpCode::CallDirectEval
                | OpCode::CallDirectEvalWithArgumentArray
                | OpCode::Debugger
        )
    });
    if changes_environment {
        return CapturedBindingAccesses::default();
    }
    let mut loads = Vec::new();
    let mut stores = Vec::new();
    for decoded in instructions {
        match decoded.instruction {
            Instruction::GetBinding { cache: coordinate, .. }
            | Instruction::GetInitializedBinding { cache: coordinate, .. }
            | Instruction::GetCalleeAndThisFromEnvironment { cache: coordinate, .. } => {
                loads.push((coordinate, decoded.pc));
            }
            Instruction::SetLexicalBinding { cache: coordinate, .. } => stores.push(coordinate),
            _ => {}
        }
    }
    CapturedBindingAccesses {
        loads: loads.into_boxed_slice(),
        stores: stores.into_boxed_slice(),
    }
}

/// Captures the snapshot of a compile job for `executable`, whose frames run `function` in `realm`, or the top-level
/// code of a script or of a module without one, in the running frame. `osr_pc` is the loop back edge whose tier-up
/// budget ran out, if a loop triggered the compile.
pub fn capture_snapshot<'d>(
    vm: &Vm,
    executable: Gc<Executable>,
    function: Option<Gc<EcmascriptFunctionObject>>,
    realm: Gc<Realm>,
    osr_pc: Option<u32>,
    defer_gc: &'d DeferGc<'_>,
) -> CapturedSnapshot<'d> {
    let compiled_function_fields = match function {
        Some(function) => function_frame_fields(function),
        None => FunctionFrameFields {
            script_or_module: script_or_module_words(vm.running_execution_context_ref()),
            ..FunctionFrameFields::default()
        },
    };
    // NB: These allocate (and so may collect garbage), which they do before anything is captured.
    let allocation = allocation_infos(vm, realm, defer_gc);
    // NB: Closures that top-level code creates have its environments, which the code does not know.
    let closure_templates = match function {
        Some(function) => closure_templates(vm, executable, function, defer_gc),
        None => defer_gc.cover((0..executable.shared_function_data_count()).map(|_| None).collect()),
    };
    let closure_samples = closure_templates
        .iter()
        .map(|template| template.as_ref().map(|(sample, _)| *sample))
        .collect::<Vec<_>>();
    let mut builder = SnapshotBuilder {
        vm,
        _defer_gc: defer_gc,
        cells: (*allocation.cells).clone(),
        entries: vec![Entry {
            executable,
            function: None,
            realm,
            depth: 0,
        }],
        indices: HashMap::new(),
        compiled_function: function,
        compiled_function_fields,
        closure_templates: Vec::new(),
        environment_templates: Vec::new(),
        bound_arguments: Vec::new(),
        lexical_environment_templates: Vec::new(),
        construct_this_shapes: HashMap::new(),
    };
    builder.closure_templates =
        closure_template_snapshots(closure_templates.into_inner(), |sample| builder.cell(sample), defer_gc);
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
        let sample_cell = sample.upcast::<CellHeader>();
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
    let mut runtime = super::runtime_info::runtime_info(vm, realm, allocation, defer_gc);
    // NB: Code that embeds the prototypes of the runtime info lists them as embedded cells.
    builder.cell(realm.array_prototype());
    builder.cell(realm.object_prototype());
    runtime.array_iterator_next = builder.optional_cell(array_iterator_next(vm, realm));
    runtime.array_iterator_return_watchpoint_set = builder.optional_cell(
        realm
            .array_iterator_return_watchpoint_set
            .get()
            .filter(|set| set.is_valid()),
    );
    // NB: Code that reads the cells of the realm embeds the realm.
    builder.cell(realm);
    runtime.array_iterator_next_watchpoint_set_address =
        realm_watchpoints::array_iterator_next_watchpoint_set_address(realm);
    runtime.array_iterator_method_watchpoint_set_address =
        realm_watchpoints::array_iterator_method_watchpoint_set_address(realm);
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
        cells: defer_gc.cover(builder.cells),
        snapshot_executables: defer_gc.cover(
            builder
                .entries
                .iter()
                .map(|entry| SnapshotExecutable {
                    executable: entry.executable,
                    function: entry.function,
                    known_exit_sites: entry.executable.jit_exit_site_count(),
                    known_builtin_exit_sites: entry.executable.jit_builtin_exit_site_count(),
                    property_cache_sizes: entry
                        .executable
                        .property_lookup_caches()
                        .iter()
                        .map(speculated_cache_size)
                        .collect(),
                })
                .collect(),
        ),
    }
}
