/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The slow paths for property access and its inline caches, and their helpers.

use core::ops::ControlFlow;
use std::collections::HashSet;

use ak::Utf16FlyString;
use libjs_abi::PutKind;
use libjs_abi::value as nan_box;

use crate::bytecode::executable::{
    ObjectPropertyIteratorCache, ObjectPropertyIteratorCacheData, ObjectPropertyIteratorFastPath, PropertyLookupCache,
    PropertyLookupCacheEntryType,
};
use crate::bytecode::op;
use crate::bytecode::operand::{IdentifierTableIndex, OptionalIndex};
use crate::bytecode::property_access::{
    self, CachePropertyAbsence, GetByIdMode, Strict, base_object_for_get, get_by_value_with_keyed_cache,
    get_cached_property_value, object_can_cache_property_additions, property_addition_is_cacheable,
    put_by_property_key,
};
use crate::gc::class::GcCell;
use crate::gc::class::class_of;
use crate::gc::root::MarkedVec;
use crate::interpreter::runtime_functions::{SlowPathControl, asm_try, handle_asm_exception};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::shape::{PrototypeChainValidity, Shape};
use crate::layout::value::Value;
use crate::runtime::abstract_operations::function_object_as_object;
use crate::runtime::array::Array;
use crate::runtime::array_buffer::{ElementType, Order};
use crate::runtime::canonical_index::{CanonicalIndex, CanonicalIndexType};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::object::{IndexedStorageKind, Object};
use crate::runtime::private_environment::{PrivateEnvironment, PrivateName};
use crate::runtime::property_attributes::{Attribute, DEFAULT_ATTRIBUTES, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::reference::Reference;
use crate::runtime::typed_array::{Kind, is_valid_integer_index, typed_array_of_object};

fn strict_of(header_strict: bool) -> Strict {
    if header_strict { Strict::Yes } else { Strict::No }
}

fn put_kind_from_operand(kind: u32) -> PutKind {
    match kind {
        0 => PutKind::Normal,
        1 => PutKind::Getter,
        2 => PutKind::Setter,
        3 => PutKind::Prototype,
        4 => PutKind::Own,
        _ => unreachable!("the bytecode only holds valid put kinds"),
    }
}

fn is_non_negative_int32(value: Value) -> bool {
    value.is_int32() && value.as_i32() >= 0
}

fn optional_identifier(vm: &Vm, index: &OptionalIndex<IdentifierTableIndex>) -> Option<Utf16FlyString> {
    let index = index.get()?;
    Some(vm.current_executable().get_identifier(index).clone())
}

fn running_private_environment(vm: &Vm) -> Gc<PrivateEnvironment> {
    let context = vm
        .running_execution_context()
        .expect("private names are used in an execution context");
    // SAFETY: The running execution context is live.
    unsafe { context.as_ref() }
        .private_environment
        .get()
        .expect("private names are used where a private environment is active")
}

#[cold]
fn throw_type_error(vm: &Vm, pc: u32, error_type: ErrorType) -> SlowPathControl {
    match vm.throw_completion::<()>(ErrorKind::TypeError, error_type, &[]) {
        Err(throw) => handle_asm_exception(vm, pc, throw.value()),
        Ok(()) => unreachable!("throw_completion always throws"),
    }
}

// 6.2.4.9 MakePrivateReference ( baseValue, privateIdentifier ), https://tc39.es/ecma262/#sec-makeprivatereference
/// The [[ReferencedName]] of the private reference, whose [[Base]] is the base value.
fn make_private_reference(vm: &Vm, private_identifier: &Utf16FlyString) -> PrivateName {
    // 1. Let privEnv be the running execution context's PrivateEnvironment.
    // 2. Assert: privEnv is not null.
    let private_environment = running_private_environment(vm);

    // 3. Let privateName be ResolvePrivateIdentifier(privEnv, privateIdentifier).
    // 4. Return the Reference Record { [[Base]]: baseValue, [[ReferencedName]]: privateName, [[Strict]]: true, [[ThisValue]]: empty }.
    private_environment.resolve_private_identifier(private_identifier)
}

/// GetValue of a private reference, as Reference::get_value does it.
// 6.2.4.5 GetValue ( V ), https://tc39.es/ecma262/#sec-getvalue
fn get_private_reference_value(vm: &Vm, base_value: Value, private_name: &PrivateName) -> ThrowCompletionOr<Value> {
    // 4. If IsPropertyReference(V) is true, then
    // a. Let baseObj be ? ToObject(V.[[Base]]).
    let base_obj = base_value.to_object(vm)?;

    // b. If IsPrivateReference(V) is true, then
    // i. Return ? PrivateGet(baseObj, V.[[ReferencedName]]).
    base_obj.private_get(vm, private_name)
}

/// PutValue of a private reference, as Reference::put_value does it.
// 6.2.4.6 PutValue ( V, W ), https://tc39.es/ecma262/#sec-putvalue
fn put_private_reference_value(
    vm: &Vm,
    base_value: Value,
    private_name: &PrivateName,
    value: Value,
) -> ThrowCompletionOr<()> {
    // 5. If IsPropertyReference(V) is true, then
    // a. Let baseObj be ? ToObject(V.[[Base]]).
    let base_obj = base_value.to_object(vm)?;

    // b. If IsPrivateReference(V) is true, then
    // i. Return ? PrivateSet(baseObj, V.[[ReferencedName]], W).
    base_obj.private_set(vm, private_name, value)
}

pub fn get_by_id(vm: &Vm, pc: u32, instruction: &op::GetById, values: &mut op::GetByIdValues) -> SlowPathControl {
    let base_value = values.base;
    let executable = vm.current_executable();
    let cache = executable.property_lookup_cache(instruction.cache as usize);
    let property_key = executable.get_property_key(instruction.property);
    let value = asm_try!(
        vm,
        pc,
        property_access::get_by_id(
            vm,
            GetByIdMode::Normal,
            || optional_identifier(vm, &instruction.base_identifier),
            &property_key,
            base_value,
            base_value,
            cache,
            CachePropertyAbsence::Yes,
        )
    );
    values.dst = value;
    SlowPathControl::continue_at(pc + op::GetById::LENGTH)
}

pub fn get_by_id_cached_accessor(
    vm: &Vm,
    pc: u32,
    instruction: &op::GetById,
    values: &mut op::GetByIdValues,
) -> SlowPathControl {
    let object = values.base.as_object();
    let executable = vm.current_executable();
    let cache = executable.property_lookup_cache(instruction.cache as usize);
    let entry = cache
        .first_entry()
        .expect("the interpreter found the accessor through the cache");

    let holder = entry.prototype.unwrap_or(object);
    let value = holder.get_direct(entry.property_offset);
    assert!(value.is_accessor());
    let getter = value.as_accessor().getter();
    let result = asm_try!(vm, pc, get_cached_property_value(vm, value, Value::from_object(object)));
    if let Some(getter) = getter
        && function_object_as_object(getter).is_direct_getter_function()
        && let Some(completed_entry) = cache.first_entry_slot()
    {
        let completed = completed_entry.get();
        if completed.shape == Some(object.shape()) {
            let completed_holder = completed.prototype.unwrap_or(object);
            let completed_value = completed_holder.get_direct(completed.property_offset);
            if completed_value.is_accessor() && completed_value.as_accessor().getter() == Some(getter) {
                completed_entry.direct_getter_validated.set(true);
            }
        }
    }
    values.dst = result;
    SlowPathControl::continue_at(pc + op::GetById::LENGTH)
}

pub fn get_by_id_with_this(
    vm: &Vm,
    pc: u32,
    instruction: &op::GetByIdWithThis,
    values: &mut op::GetByIdWithThisValues,
) -> SlowPathControl {
    let base_value = values.base;
    let this_value = values.this_value;
    let executable = vm.current_executable();
    let cache = executable.property_lookup_cache(instruction.cache as usize);
    let property_key = executable.get_property_key(instruction.property);
    let value = asm_try!(
        vm,
        pc,
        property_access::get_by_id(
            vm,
            GetByIdMode::Normal,
            || None,
            &property_key,
            base_value,
            this_value,
            cache,
            CachePropertyAbsence::No,
        )
    );
    values.dst = value;
    SlowPathControl::continue_at(pc + op::GetByIdWithThis::LENGTH)
}

pub fn put_by_id(vm: &Vm, pc: u32, instruction: &op::PutById, values: &mut op::PutByIdValues) -> SlowPathControl {
    let value = values.src;
    let base = values.base;
    let executable = vm.current_executable();
    let property_key = executable.get_property_key(instruction.property);
    let cache = executable.property_lookup_cache(instruction.cache as usize);
    asm_try!(
        vm,
        pc,
        put_by_property_key(
            vm,
            base,
            base,
            value,
            || optional_identifier(vm, &instruction.base_identifier),
            &property_key,
            put_kind_from_operand(instruction.kind),
            strict_of(instruction.header.strict),
            Some(cache),
        )
    );
    SlowPathControl::continue_at(pc + op::PutById::LENGTH)
}

pub fn put_by_id_with_this(
    vm: &Vm,
    pc: u32,
    instruction: &op::PutByIdWithThis,
    values: &mut op::PutByIdWithThisValues,
) -> SlowPathControl {
    let value = values.src;
    let base = values.base;
    let executable = vm.current_executable();
    let name = executable.get_property_key(instruction.property);
    let cache = executable.property_lookup_cache(instruction.cache as usize);
    asm_try!(
        vm,
        pc,
        put_by_property_key(
            vm,
            base,
            values.this_value,
            value,
            || None,
            &name,
            put_kind_from_operand(instruction.kind),
            strict_of(instruction.header.strict),
            Some(cache),
        )
    );
    SlowPathControl::continue_at(pc + op::PutByIdWithThis::LENGTH)
}

pub fn get_by_value(
    vm: &Vm,
    pc: u32,
    instruction: &op::GetByValue,
    values: &mut op::GetByValueValues,
) -> SlowPathControl {
    let base_value = values.base;
    let property_key_value = values.property;
    let object = asm_try!(
        vm,
        pc,
        base_object_for_get(
            vm,
            base_value,
            || optional_identifier(vm, &instruction.base_identifier),
            &property_key_value,
        )
    );
    let property_key = asm_try!(vm, pc, property_key_value.to_property_key(vm));
    if base_value.is_string() {
        let string_value = asm_try!(vm, pc, base_value.as_string().get(vm, &property_key));
        if let Some(string_value) = string_value {
            values.dst = string_value;
            return SlowPathControl::continue_at(pc + op::GetByValue::LENGTH);
        }
    }
    values.dst = asm_try!(
        vm,
        pc,
        get_by_value_with_keyed_cache(vm, object, base_value, &property_key)
    );
    SlowPathControl::continue_at(pc + op::GetByValue::LENGTH)
}

pub fn get_by_value_with_this(vm: &Vm, pc: u32, values: &mut op::GetByValueWithThisValues) -> SlowPathControl {
    let property_key_value = values.property;
    let object = asm_try!(vm, pc, values.base.to_object(vm));
    let property_key = asm_try!(vm, pc, property_key_value.to_property_key(vm));
    let value = asm_try!(
        vm,
        pc,
        get_by_value_with_keyed_cache(vm, object, values.this_value, &property_key)
    );
    values.dst = value;
    SlowPathControl::continue_at(pc + op::GetByValueWithThis::LENGTH)
}

fn length_property_key(vm: &Vm) -> PropertyKey {
    let executable = vm.current_executable();
    executable.get_property_key(
        executable
            .length_identifier
            .expect("an executable that reads a length has the length in its property key table"),
    )
}

pub fn get_length(vm: &Vm, pc: u32, instruction: &op::GetLength, values: &mut op::GetLengthValues) -> SlowPathControl {
    let base_value = values.base;
    let executable = vm.current_executable();
    let cache = executable.property_lookup_cache(instruction.cache as usize);
    let value = asm_try!(
        vm,
        pc,
        property_access::get_by_id(
            vm,
            GetByIdMode::Length,
            || optional_identifier(vm, &instruction.base_identifier),
            &length_property_key(vm),
            base_value,
            base_value,
            cache,
            CachePropertyAbsence::No,
        )
    );
    values.dst = value;
    SlowPathControl::continue_at(pc + op::GetLength::LENGTH)
}

pub fn get_length_with_this(
    vm: &Vm,
    pc: u32,
    instruction: &op::GetLengthWithThis,
    values: &mut op::GetLengthWithThisValues,
) -> SlowPathControl {
    let base_value = values.base;
    let this_value = values.this_value;
    let executable = vm.current_executable();
    let cache = executable.property_lookup_cache(instruction.cache as usize);
    let value = asm_try!(
        vm,
        pc,
        property_access::get_by_id(
            vm,
            GetByIdMode::Length,
            || None,
            &length_property_key(vm),
            base_value,
            this_value,
            cache,
            CachePropertyAbsence::No,
        )
    );
    values.dst = value;
    SlowPathControl::continue_at(pc + op::GetLengthWithThis::LENGTH)
}

pub fn get_method(vm: &Vm, pc: u32, instruction: &op::GetMethod, values: &mut op::GetMethodValues) -> SlowPathControl {
    let property_key = vm.current_executable().get_property_key(instruction.property);
    let method = asm_try!(vm, pc, values.object.get_method(vm, &property_key));
    values.dst = method.map_or(Value::UNDEFINED, Value::from_object);
    SlowPathControl::continue_at(pc + op::GetMethod::LENGTH)
}

pub fn put_by_value(
    vm: &Vm,
    pc: u32,
    instruction: &op::PutByValue,
    values: &mut op::PutByValueValues,
) -> SlowPathControl {
    let value = values.src;
    let base = values.base;
    let property = values.property;
    let property_key = asm_try!(vm, pc, property.to_property_key(vm));
    asm_try!(
        vm,
        pc,
        put_by_property_key(
            vm,
            base,
            base,
            value,
            || optional_identifier(vm, &instruction.base_identifier),
            &property_key,
            put_kind_from_operand(instruction.kind),
            strict_of(instruction.header.strict),
            None,
        )
    );
    SlowPathControl::continue_at(pc + op::PutByValue::LENGTH)
}

pub fn put_by_value_with_this(
    vm: &Vm,
    pc: u32,
    instruction: &op::PutByValueWithThis,
    values: &mut op::PutByValueWithThisValues,
) -> SlowPathControl {
    let value = values.src;
    let base = values.base;
    let this_value = values.this_value;
    let property_key = asm_try!(vm, pc, values.property.to_property_key(vm));
    asm_try!(
        vm,
        pc,
        put_by_property_key(
            vm,
            base,
            this_value,
            value,
            || None,
            &property_key,
            put_kind_from_operand(instruction.kind),
            strict_of(instruction.header.strict),
            None,
        )
    );
    SlowPathControl::continue_at(pc + op::PutByValueWithThis::LENGTH)
}

pub fn put_by_spread(vm: &Vm, pc: u32, values: &mut op::PutBySpreadValues) -> SlowPathControl {
    let value = values.src;
    let base = values.base;

    // a. Let baseObj be ? ToObject(V.[[Base]]).
    let object = asm_try!(vm, pc, base.to_object(vm));

    asm_try!(
        vm,
        pc,
        object.copy_data_properties(vm, value, &MarkedVec::new(vm), &MarkedVec::new(vm))
    );
    SlowPathControl::continue_at(pc + op::PutBySpread::LENGTH)
}

pub fn delete_by_id(
    vm: &Vm,
    pc: u32,
    instruction: &op::DeleteById,
    values: &mut op::DeleteByIdValues,
) -> SlowPathControl {
    let property_key = vm.current_executable().get_property_key(instruction.property);
    let result = asm_try!(
        vm,
        pc,
        Reference::with_base_value(values.base, property_key, None, strict_of(instruction.header.strict)).delete_(vm)
    );
    values.dst = Value::from_bool(result);
    SlowPathControl::continue_at(pc + op::DeleteById::LENGTH)
}

pub fn delete_by_value(
    vm: &Vm,
    pc: u32,
    instruction: &op::DeleteByValue,
    values: &mut op::DeleteByValueValues,
) -> SlowPathControl {
    let property_key = asm_try!(vm, pc, values.property.to_property_key(vm));
    let result = asm_try!(
        vm,
        pc,
        Reference::with_base_value(values.base, property_key, None, strict_of(instruction.header.strict)).delete_(vm)
    );
    values.dst = Value::from_bool(result);
    SlowPathControl::continue_at(pc + op::DeleteByValue::LENGTH)
}

pub fn copy_object_excluding_properties(
    vm: &Vm,
    pc: u32,
    instruction: &op::CopyObjectExcludingProperties,
    values: &mut op::CopyObjectExcludingPropertiesValues,
    excluded_names: &[Value],
) -> SlowPathControl {
    let realm = vm.current_realm().expect("there is a current realm");
    let from_object = values.from_object;
    let to_object = Object::create(vm, realm, Some(realm.object_prototype()));

    let excluded_name_keys = MarkedVec::with_capacity(vm, excluded_names.len());
    for &excluded_name in excluded_names {
        excluded_name_keys.push(asm_try!(vm, pc, excluded_name.to_property_key(vm)));
    }

    asm_try!(
        vm,
        pc,
        to_object.copy_data_properties(vm, from_object, &excluded_name_keys, &MarkedVec::new(vm))
    );
    values.dst = Value::from_object(to_object);
    SlowPathControl::continue_at(pc + instruction.length())
}

pub fn create_data_property_or_throw(
    vm: &Vm,
    pc: u32,
    values: &mut op::CreateDataPropertyOrThrowValues,
) -> SlowPathControl {
    let object = values.object.as_object();
    let property = asm_try!(vm, pc, values.property.to_property_key(vm));
    let value = values.value;
    asm_try!(vm, pc, object.create_data_property_or_throw(vm, &property, value));
    SlowPathControl::continue_at(pc + op::CreateDataPropertyOrThrow::LENGTH)
}

pub fn new_object(vm: &Vm, pc: u32, instruction: &op::NewObject, values: &mut op::NewObjectValues) -> SlowPathControl {
    let realm = vm.current_realm().expect("there is a current realm");

    if instruction.cache != u32::MAX {
        let executable = vm.current_executable();
        let cache = executable.object_shape_cache(instruction.cache);
        if let Some(cached_shape) = cache.shape.get() {
            values.dst = Value::from_object(Object::create_with_premade_shape(vm, cached_shape));
            return SlowPathControl::continue_at(pc + op::NewObject::LENGTH);
        }
    }

    values.dst = Value::from_object(Object::create(vm, realm, Some(realm.object_prototype())));
    SlowPathControl::continue_at(pc + op::NewObject::LENGTH)
}

pub fn new_object_with_no_prototype(
    vm: &Vm,
    pc: u32,
    values: &mut op::NewObjectWithNoPrototypeValues,
) -> SlowPathControl {
    let realm = vm.current_realm().expect("there is a current realm");
    values.dst = Value::from_object(Object::create(vm, realm, None));
    SlowPathControl::continue_at(pc + op::NewObjectWithNoPrototype::LENGTH)
}

pub fn cache_object_shape(
    vm: &Vm,
    pc: u32,
    instruction: &op::CacheObjectShape,
    values: &mut op::CacheObjectShapeValues,
) -> SlowPathControl {
    let executable = vm.current_executable();
    let cache = executable.object_shape_cache(instruction.cache);
    if cache.shape.get().is_none() {
        let object = values.object.as_object();
        if !object.shape().is_dictionary() {
            cache.shape.set(Some(object.shape()));
        }
    }
    SlowPathControl::continue_at(pc + op::CacheObjectShape::LENGTH)
}

pub fn init_object_literal_property(
    vm: &Vm,
    pc: u32,
    instruction: &op::InitObjectLiteralProperty,
    values: &mut op::InitObjectLiteralPropertyValues,
) -> SlowPathControl {
    let object = values.object.as_object();
    let value = values.src;
    let executable = vm.current_executable();
    let cache = executable.object_shape_cache(instruction.shape_cache_index);
    let property_slot = instruction.property_slot as usize;

    let cached_shape = cache.shape.get();
    let cached_property_offset = cache.property_offsets.borrow().get(property_slot).copied();
    if let Some(cached_shape) = cached_shape
        && object.shape() == cached_shape
        && let Some(property_offset) = cached_property_offset
    {
        object.put_direct(property_offset, value);
        return SlowPathControl::continue_at(pc + op::InitObjectLiteralProperty::LENGTH);
    }

    let property_key = executable.get_property_key(instruction.property);
    object.define_direct_property(
        vm,
        &property_key,
        value,
        PropertyAttributes::new(Attribute::ENUMERABLE | Attribute::WRITABLE | Attribute::CONFIGURABLE),
    );

    if !object.shape().is_dictionary()
        && let Some(metadata) = object.shape().lookup(&property_key)
    {
        let mut property_offsets = cache.property_offsets.borrow_mut();
        if property_slot >= property_offsets.len() {
            property_offsets.resize(property_slot + 1, 0);
        }
        property_offsets[property_slot] = metadata.offset;
    }

    SlowPathControl::continue_at(pc + op::InitObjectLiteralProperty::LENGTH)
}

pub fn has_private_id(
    vm: &Vm,
    pc: u32,
    instruction: &op::HasPrivateId,
    values: &mut op::HasPrivateIdValues,
) -> SlowPathControl {
    let base = values.base;
    if !base.is_object() {
        return throw_type_error(vm, pc, ErrorType::InOperatorWithObject);
    }

    let private_environment = running_private_environment(vm);
    let private_name =
        private_environment.resolve_private_identifier(vm.current_executable().get_identifier(instruction.property));
    values.dst = Value::from_bool(base.as_object().private_element_find(&private_name).is_some());
    SlowPathControl::continue_at(pc + op::HasPrivateId::LENGTH)
}

pub fn add_private_name(vm: &Vm, pc: u32, instruction: &op::AddPrivateName) -> SlowPathControl {
    let name = vm.current_executable().get_identifier(instruction.name).clone();
    running_private_environment(vm).add_private_name(name);
    SlowPathControl::continue_at(pc + op::AddPrivateName::LENGTH)
}

// Direct handler for GetPrivateById: bypasses Reference indirection.
pub fn get_private_by_id(
    vm: &Vm,
    pc: u32,
    instruction: &op::GetPrivateById,
    values: &mut op::GetPrivateByIdValues,
) -> SlowPathControl {
    let base_value = values.base;

    if !base_value.is_object() {
        asm_try!(vm, pc, base_value.to_object(vm));
        let name = vm.current_executable().get_identifier(instruction.property).clone();
        let private_name = make_private_reference(vm, &name);
        let result = asm_try!(vm, pc, get_private_reference_value(vm, base_value, &private_name));
        values.dst = result;
        return SlowPathControl::continue_at(pc + op::GetPrivateById::LENGTH);
    }

    let name = vm.current_executable().get_identifier(instruction.property).clone();
    let private_environment = running_private_environment(vm);
    let private_name = private_environment.resolve_private_identifier(&name);
    let result = asm_try!(vm, pc, base_value.as_object().private_get(vm, &private_name));
    values.dst = result;
    SlowPathControl::continue_at(pc + op::GetPrivateById::LENGTH)
}

// Direct handler for PutPrivateById: bypasses Reference indirection.
pub fn put_private_by_id(
    vm: &Vm,
    pc: u32,
    instruction: &op::PutPrivateById,
    values: &mut op::PutPrivateByIdValues,
) -> SlowPathControl {
    let base_value = values.base;
    let value = values.src;

    if !base_value.is_object() {
        let object = asm_try!(vm, pc, base_value.to_object(vm));
        let name = vm.current_executable().get_identifier(instruction.property).clone();
        let private_name = make_private_reference(vm, &name);
        asm_try!(
            vm,
            pc,
            put_private_reference_value(vm, Value::from_object(object), &private_name, value)
        );
        return SlowPathControl::continue_at(pc + op::PutPrivateById::LENGTH);
    }

    let name = vm.current_executable().get_identifier(instruction.property).clone();
    let private_environment = running_private_environment(vm);
    let private_name = private_environment.resolve_private_identifier(&name);
    asm_try!(vm, pc, base_value.as_object().private_set(vm, &private_name, value));
    SlowPathControl::continue_at(pc + op::PutPrivateById::LENGTH)
}

/// What a fast for-in snapshot is built from, as FastPropertyNameIteratorData in SlowPaths.cpp.
struct FastPropertyNameIteratorData<'vm> {
    properties: MarkedVec<'vm, PropertyKey>,
    fast_path: ObjectPropertyIteratorFastPath,
    indexed_property_count: u32,
    receiver_has_magical_length_property: bool,
    shape: Gc<Shape>,
    prototype_chain_validity: Option<Gc<PrototypeChainValidity>>,
}

fn shape_has_enumerable_string_property(shape: &Shape) -> bool {
    let mut has_enumerable_string_property = false;
    shape.for_each_property_in_insertion_order(|property_key, metadata| {
        if property_key.is_string() && metadata.attributes.is_enumerable() {
            has_enumerable_string_property = true;
            return ControlFlow::Break(());
        }
        ControlFlow::Continue(())
    });
    has_enumerable_string_property
}

fn property_name_iterator_fast_path_is_still_eligible(
    object: Gc<Object>,
    fast_path: ObjectPropertyIteratorFastPath,
    indexed_property_count: u32,
) -> bool {
    let mut object_to_check = Some(object);
    let mut is_receiver = true;

    while let Some(current) = object_to_check {
        if !current.eligible_for_own_property_enumeration_fast_path() {
            return false;
        }

        if is_receiver {
            if fast_path == ObjectPropertyIteratorFastPath::PackedIndexed {
                if current.indexed_storage_kind() != IndexedStorageKind::Packed {
                    return false;
                }
                if current.indexed_array_like_size() != indexed_property_count {
                    return false;
                }
            } else if current.indexed_array_like_size() != 0 {
                return false;
            }
        } else if current.indexed_array_like_size() != 0 {
            return false;
        }

        object_to_check = current.prototype();
        is_receiver = false;
    }

    true
}

fn object_property_iterator_cache_matches(object: Gc<Object>, cache: &ObjectPropertyIteratorCacheData) -> bool {
    // A cache entry represents the fully flattened key snapshot for one bytecode
    // site. Reusing it is only valid while the receiver still has the same local
    // state and the prototype chain validity token says nothing above it changed.
    if object.has_magical_length_property() != cache.receiver_has_magical_length_property() {
        return false;
    }

    let shape = object.shape();
    if Some(shape) != cache.shape() {
        return false;
    }

    if shape.is_dictionary() && shape.dictionary_generation() != cache.shape_dictionary_generation() {
        return false;
    }

    if cache
        .prototype_chain_validity()
        .is_some_and(|validity| !validity.is_valid())
    {
        return false;
    }

    property_name_iterator_fast_path_is_still_eligible(object, cache.fast_path(), cache.indexed_property_count())
}

/// The objects a walk up a prototype chain has visited, kept alive so that their addresses stay theirs.
struct SeenObjects<'vm> {
    objects: MarkedVec<'vm, Gc<Object>>,
    addresses: HashSet<usize>,
}

impl<'vm> SeenObjects<'vm> {
    fn new(vm: &'vm Vm) -> Self {
        Self {
            objects: MarkedVec::new(vm),
            addresses: HashSet::new(),
        }
    }

    fn contains(&self, object: Gc<Object>) -> bool {
        self.addresses.contains(&object.as_ptr().addr())
    }

    fn set(&mut self, object: Gc<Object>) {
        if self.addresses.insert(object.as_ptr().addr()) {
            self.objects.push(object);
        }
    }

    fn clear(&mut self) {
        self.addresses.clear();
        while self.objects.pop().is_some() {}
    }
}

/// The keys a for-in has already decided about while it walks up the prototype chain. The keys are never symbols,
/// so a plain set may hold them.
struct ShadowingState {
    seen_non_enumerable_properties: HashSet<PropertyKey>,
    seen_properties: Option<HashSet<PropertyKey>>,
}

impl ShadowingState {
    fn new() -> Self {
        Self {
            seen_non_enumerable_properties: HashSet::new(),
            seen_properties: None,
        }
    }

    fn ensure_seen_properties(&mut self, properties: &MarkedVec<'_, PropertyKey>) -> &mut HashSet<PropertyKey> {
        self.seen_properties.get_or_insert_with(|| {
            // Prototype shadowing ignores enumerability, so once we start looking
            // above the receiver we need an explicit visited set for names we have
            // already decided to expose from lower objects.
            let mut seen_properties = HashSet::with_capacity(properties.len());
            for index in 0..properties.len() {
                seen_properties.insert(properties.get(index).expect("the index is in bounds"));
            }
            seen_properties
        })
    }

    /// Decides about one key of an object in the chain, appending it to `properties` if the for-in visits it.
    fn visit(
        &mut self,
        properties: &MarkedVec<'_, PropertyKey>,
        property_key: &PropertyKey,
        enumerable: bool,
        in_prototype_chain: bool,
    ) {
        if !enumerable {
            self.seen_non_enumerable_properties.insert(property_key.clone());
        }
        if in_prototype_chain && enumerable {
            if self.seen_non_enumerable_properties.contains(property_key) {
                return;
            }
            if self.ensure_seen_properties(properties).contains(property_key) {
                return;
            }
        }
        if enumerable {
            properties.push(property_key.clone());
        }
        if let Some(seen_properties) = &mut self.seen_properties {
            seen_properties.insert(property_key.clone());
        }
    }
}

fn try_get_fast_property_name_iterator_data(
    vm: &Vm,
    object: Gc<Object>,
) -> ThrowCompletionOr<Option<FastPropertyNameIteratorData<'_>>> {
    let mut fast_path = ObjectPropertyIteratorFastPath::PlainNamed;
    let mut indexed_property_count = 0;
    let receiver_has_magical_length_property = object.has_magical_length_property();
    let shape = object.shape();

    let mut seen_objects = SeenObjects::new(vm);
    let mut estimated_properties_count = 0usize;
    let mut prototype_chain_has_enumerable_named_properties = false;
    let mut object_to_check = Some(object);
    while let Some(current) = object_to_check
        && !seen_objects.contains(current)
    {
        seen_objects.set(current);
        if !current.eligible_for_own_property_enumeration_fast_path() {
            return Ok(None);
        }
        if current == object {
            if current.indexed_array_like_size() != 0 {
                if current.indexed_storage_kind() != IndexedStorageKind::Packed {
                    return Ok(None);
                }
                fast_path = ObjectPropertyIteratorFastPath::PackedIndexed;
                indexed_property_count = current.indexed_array_like_size();
            } else {
                fast_path = ObjectPropertyIteratorFastPath::PlainNamed;
            }
        } else if current.indexed_array_like_size() != 0 {
            // The fast path only knows how to synthesize a packed indexed prefix
            // for the receiver itself. As soon as indexed properties appear in
            // the prototype chain, we fall back to the generic enumeration path.
            return Ok(None);
        } else if !prototype_chain_has_enumerable_named_properties {
            prototype_chain_has_enumerable_named_properties = shape_has_enumerable_string_property(&current.shape());
        }
        estimated_properties_count += current.shape().property_count() as usize;
        object_to_check = current.internal_get_prototype_of(vm)?;
    }
    seen_objects.clear();

    let mut prototype_chain_validity = None;
    if let Some(prototype) = object.shape().prototype() {
        prototype_chain_validity = prototype.shape().prototype_chain_validity();
        if prototype_chain_validity.is_none() {
            return Ok(None);
        }
    }

    let make_result = |properties| FastPropertyNameIteratorData {
        properties,
        fast_path,
        indexed_property_count,
        receiver_has_magical_length_property,
        shape,
        prototype_chain_validity,
    };

    if !prototype_chain_has_enumerable_named_properties {
        // Common case: only the receiver contributes enumerable string keys, so
        // we can copy them straight from the shape without any shadowing work.
        let properties = MarkedVec::with_capacity(vm, object.shape().property_count() as usize);
        object
            .shape()
            .for_each_property_in_insertion_order(|property_key, metadata| {
                if property_key.is_string() && metadata.attributes.is_enumerable() {
                    properties.push(property_key.clone());
                }
                ControlFlow::Continue(())
            });
        return Ok(Some(make_result(properties)));
    }

    let properties = MarkedVec::with_capacity(vm, estimated_properties_count);
    let mut shadowing = ShadowingState::new();

    let mut in_prototype_chain = false;
    let mut object_to_check = Some(object);
    while let Some(current) = object_to_check
        && !seen_objects.contains(current)
    {
        seen_objects.set(current);

        // Arrays keep a non-enumerable magical `length` property outside the shape
        // table, but it still shadows enumerable `length` properties higher up the
        // prototype chain during for-in.
        if current.has_magical_length_property() {
            shadowing.seen_non_enumerable_properties.insert(vm.names.length.clone());
        }

        current
            .shape()
            .for_each_property_in_insertion_order(|property_key, metadata| {
                if !property_key.is_string() {
                    return ControlFlow::Continue(());
                }
                shadowing.visit(
                    &properties,
                    property_key,
                    metadata.attributes.is_enumerable(),
                    in_prototype_chain,
                );
                ControlFlow::Continue(())
            });
        in_prototype_chain = true;
        object_to_check = current.internal_get_prototype_of(vm)?;
    }

    Ok(Some(make_result(properties)))
}

// 14.7.5.9 EnumerateObjectProperties ( O ), https://tc39.es/ecma262/#sec-enumerate-object-properties
fn get_object_property_iterator_impl(
    vm: &Vm,
    object: Gc<Object>,
    cache: Option<&ObjectPropertyIteratorCache>,
) -> ThrowCompletionOr<Gc<ObjectPropertyIteratorCacheData>> {
    // While the spec does provide an algorithm, it allows us to implement it ourselves so long as we meet the following invariants:
    //    1- Returned property keys do not include keys that are Symbols
    //    2- Properties of the target object may be deleted during enumeration. A property that is deleted before it is processed by the iterator's next method is ignored
    //    3- If new properties are added to the target object during enumeration, the newly added properties are not guaranteed to be processed in the active enumeration
    //    4- A property name will be returned by the iterator's next method at most once in any enumeration.
    //    5- Enumerating the properties of the target object includes enumerating properties of its prototype, and the prototype of the prototype, and so on, recursively;
    //       but a property of a prototype is not processed if it has the same name as a property that has already been processed by the iterator's next method.
    //    6- The values of [[Enumerable]] attributes are not considered when determining if a property of a prototype object has already been processed.
    //    7- The enumerable property names of prototype objects must be obtained by invoking EnumerateObjectProperties passing the prototype object as the argument.
    //    8- EnumerateObjectProperties must obtain the own property keys of the target object by calling its [[OwnPropertyKeys]] internal method.
    //    9- Property attributes of the target object must be obtained by calling its [[GetOwnProperty]] internal method

    // Invariant 3 effectively allows the implementation to ignore newly added keys, and we do so (similar to other implementations).
    // Note: While the spec doesn't explicitly require these to be ordered, it says that the values should be retrieved via OwnPropertyKeys,
    //       so we just keep the order consistent anyway.

    if let Some(cache) = cache
        && let Some(data) = cache.data.get()
    {
        // The flattened key snapshot for this site is still valid, so reuse it as-is. The per-loop
        // iteration state (the cursor) lives in a bytecode register, not in this cell.
        if object_property_iterator_cache_matches(object, &data) {
            return Ok(data);
        }
    }

    // Keep a snapshot on the shape so sites that alternate between shapes can reuse
    // previously collected keys instead of rebuilding the list each time.
    if let Some(shape_cache) = object.shape().property_iterator_cache()
        && object_property_iterator_cache_matches(object, &shape_cache)
    {
        if let Some(cache) = cache {
            cache.data.set(Some(shape_cache));
        }
        return Ok(shape_cache);
    }

    if let Some(fast_iterator_data) = try_get_fast_property_name_iterator_data(vm, object)? {
        let cache_data = ObjectPropertyIteratorCacheData::create_with_fast_path(
            vm,
            &fast_iterator_data.properties,
            fast_iterator_data.fast_path,
            fast_iterator_data.indexed_property_count,
            fast_iterator_data.receiver_has_magical_length_property,
            fast_iterator_data.shape,
            fast_iterator_data.prototype_chain_validity,
        );
        if let Some(cache) = cache {
            cache.data.set(Some(cache_data));
        }
        object.shape().set_property_iterator_cache(cache_data);
        return Ok(cache_data);
    }

    let mut estimated_properties_count = 0;
    let mut seen_objects = SeenObjects::new(vm);
    let mut object_to_check = Some(object);
    while let Some(current) = object_to_check
        && !seen_objects.contains(current)
    {
        seen_objects.set(current);
        estimated_properties_count += current.own_properties_count();
        object_to_check = current.internal_get_prototype_of(vm)?;
    }
    seen_objects.clear();

    let properties = MarkedVec::with_capacity(vm, estimated_properties_count);
    let mut shadowing = ShadowingState::new();

    // Collect all keys immediately (invariant no. 5)
    let mut in_prototype_chain = false;
    let mut object_to_check = Some(object);
    while let Some(current) = object_to_check
        && !seen_objects.contains(current)
    {
        seen_objects.set(current);
        current.for_each_own_property_with_enumerability(vm, |property_key, enumerable| {
            shadowing.visit(&properties, property_key, enumerable, in_prototype_chain);
            Ok(())
        })?;
        in_prototype_chain = true;
        object_to_check = current.internal_get_prototype_of(vm)?;
    }

    // A slow-path snapshot has no fast path to revalidate; enumeration filters deleted keys with
    // has_property() at each step. It is not cached on the site, because the key set depends on the
    // receiver rather than only its shape.
    Ok(ObjectPropertyIteratorCacheData::create(vm, &properties))
}

pub fn get_object_property_iterator(
    vm: &Vm,
    pc: u32,
    instruction: &op::GetObjectPropertyIterator,
    values: &mut op::GetObjectPropertyIteratorValues,
) -> SlowPathControl {
    let executable = vm.current_executable();
    let cache = executable.object_property_iterator_cache(instruction.cache);
    // ToObject the enumeration source once here. The boxed receiver, not the raw source value, is
    // what ObjectPropertyIteratorNext revalidates against and calls has_property() on.
    let receiver = asm_try!(vm, pc, values.object.to_object(vm));
    let keys = asm_try!(vm, pc, get_object_property_iterator_impl(vm, receiver, Some(cache)));
    values.dst_keys = Value::with_cell_tag(nan_box::IS_CELL_BIT, keys);
    values.dst_receiver = Value::from_object(receiver);
    SlowPathControl::continue_at(pc + op::GetObjectPropertyIterator::LENGTH)
}

// Advance a for-in enumeration by one step. The receiver, its flattened key snapshot, and the cursor
// are all passed in explicitly; there is no iterator object. This is the slow companion to the flap
// fast path, reached once the snapshot's shape guards no longer hold (or never held, for a snapshot
// with no fast path), so it filters every remaining key with has_property() the way the spec's
// deleted-property invariant requires.
fn object_property_iterator_next_step(
    vm: &Vm,
    receiver: Gc<Object>,
    keys: Gc<ObjectPropertyIteratorCacheData>,
    cursor: &mut usize,
    done: &mut bool,
    value: &mut Value,
) -> ThrowCompletionOr<()> {
    let indexed_count = keys.indexed_property_count() as usize;
    let total = indexed_count
        .checked_add(keys.property_count())
        .expect("the key count fits in usize");

    while *cursor < total {
        let current = *cursor;
        *cursor += 1;
        let entry = if current < indexed_count {
            PropertyKey::from(current as u32)
        } else {
            keys.property(current - indexed_count)
        };

        // Invariant 2: a property deleted before the iterator reaches it is skipped.
        if !receiver.has_property(vm, &entry)? {
            continue;
        }

        *done = false;
        *value = entry.to_value(vm);
        return Ok(());
    }

    *done = true;
    Ok(())
}

/// The key snapshot GetObjectPropertyIterator stored in a register, as a value holding a cell.
fn object_property_iterator_cache_data_of(value: Value) -> Gc<ObjectPropertyIteratorCacheData> {
    assert!(value.tag() == nan_box::IS_CELL_BIT);
    // SAFETY: The tag says the value holds a cell, whose class is checked next.
    let cell = unsafe { value.cell::<ObjectPropertyIteratorCacheData>() };
    assert!(class_of(cell).is_subclass_of(ObjectPropertyIteratorCacheData::CLASS));
    cell
}

pub fn object_property_iterator_next(
    vm: &Vm,
    pc: u32,
    values: &mut op::ObjectPropertyIteratorNextValues,
) -> SlowPathControl {
    let receiver = values.receiver.as_object();
    let keys = object_property_iterator_cache_data_of(values.keys);
    // The cursor is a Number that only for-in codegen and this op ever write: an int32 while it fits,
    // and a double once it does not. The fast path only handles the int32 case, so a snapshot larger
    // than the int32 range finishes here. Value(double) narrows back to int32 whenever possible.
    assert!(values.cursor.is_integral_number());
    let cursor_number = values.cursor.as_f64();
    assert!(cursor_number >= 0.0 && cursor_number <= usize::MAX as f64);
    let mut cursor = cursor_number as usize;
    let mut value = Value::UNDEFINED;
    let mut done = false;
    asm_try!(
        vm,
        pc,
        object_property_iterator_next_step(vm, receiver, keys, &mut cursor, &mut done, &mut value)
    );
    values.dst_done = Value::from_bool(done);
    values.dst_value = value;
    values.cursor = Value::from_f64(cursor as f64);
    SlowPathControl::continue_at(pc + op::ObjectPropertyIteratorNext::LENGTH)
}

pub fn try_put_by_value_holey_array(values: &op::PutByValueValues) -> bool {
    let base = values.base;
    if !base.is_object() {
        return false;
    }

    let property = values.property;
    if !is_non_negative_int32(property) {
        return false;
    }

    let object = base.as_object();
    let Some(array) = object.downcast::<Array>() else {
        return false;
    };

    if array.is_proxy_target()
        || !array.default_prototype_chain_intact()
        || !array.extensible()
        || array.may_interfere_with_indexed_property_access()
        || array.indexed_storage_kind() != IndexedStorageKind::Holey
    {
        return false;
    }

    let index = property.as_i32() as u32;
    if index >= array.indexed_array_like_size() {
        return false;
    }

    array.indexed_put(index, values.src, DEFAULT_ATTRIBUTES);
    true
}

pub fn try_inline_get_by_id_accessor(vm: &Vm, pc: u32, instruction: &op::GetById, values: &op::GetByIdValues) -> bool {
    let object = values.base.as_object();
    let executable = vm.current_executable();
    let cache = executable.property_lookup_cache(instruction.cache as usize);
    let entry = cache
        .first_entry()
        .expect("the interpreter found the accessor through the cache");

    let holder = entry.prototype.unwrap_or(object);
    let value = holder.get_direct(entry.property_offset);
    assert!(value.is_accessor());

    let Some(getter) = value.as_accessor().getter() else {
        return false;
    };
    let Some(getter_function) = as_ecmascript_function_object(getter) else {
        return false;
    };

    if !getter_function.can_inline_call() {
        return false;
    }

    vm.push_inline_frame(
        getter_function,
        getter_function.inline_call_executable(),
        &[],
        pc + instruction.length(),
        instruction.dst.0,
        Value::from_object(object),
        None,
        false,
    )
    .is_some()
}

// Fast cache-only PutById. Tries all cache entries for ChangeOwnProperty and
// AddOwnProperty. Returns whether the cache handled the put; if not, the caller
// uses the full slow path.
pub fn try_put_by_id_cache(vm: &Vm, instruction: &op::PutById, values: &op::PutByIdValues) -> bool {
    let base = values.base;
    if !base.is_object() {
        return false;
    }
    let object = base.as_object();
    let value = values.src;
    let executable = vm.current_executable();
    let cache = executable.property_lookup_cache(instruction.cache as usize);

    for entry in cache.entries_for_shape(object.shape()).as_slice() {
        match entry.entry_type {
            PropertyLookupCacheEntryType::ChangeOwnProperty => {
                let Some(cached_shape) = entry.shape else {
                    continue;
                };
                if cached_shape != object.shape() {
                    continue;
                }
                if cached_shape.is_dictionary()
                    && cached_shape.dictionary_generation() != entry.shape_dictionary_generation
                {
                    continue;
                }
                let current = object.get_direct(entry.property_offset);
                if current.is_accessor() || !entry.writes_data_property {
                    return false;
                }
                object.put_direct(entry.property_offset, value);
                return true;
            }
            PropertyLookupCacheEntryType::AddOwnProperty => {
                if entry.from_shape != Some(object.shape()) {
                    continue;
                }
                if !object_can_cache_property_additions(&object) {
                    continue;
                }
                if object.has_magical_length_property()
                    && !property_addition_is_cacheable(vm, &object, &executable.get_property_key(instruction.property))
                {
                    continue;
                }
                let Some(cached_shape) = entry.shape else {
                    continue;
                };
                if !object.extensible() {
                    continue;
                }
                if cached_shape.is_dictionary()
                    && object.shape().dictionary_generation() != entry.shape_dictionary_generation
                {
                    continue;
                }
                if entry
                    .prototype_chain_validity
                    .is_some_and(|validity| !validity.is_valid())
                {
                    continue;
                }
                object.unsafe_set_shape(cached_shape);
                object.put_direct(entry.property_offset, value);
                return true;
            }
            _ => continue,
        }
    }
    false
}

// Fast cache-only GetById. Tries all cache entries for own-property and prototype
// chain lookups. Returns the cached value on hit, or Empty on miss.
pub fn try_get_by_id_cache(base: Value, cache: &PropertyLookupCache) -> Value {
    if !base.is_object() {
        return Value::EMPTY;
    }
    let object = base.as_object();
    let shape = object.shape();

    for entry in cache.entries_for_shape(shape).as_slice() {
        if entry.entry_type == PropertyLookupCacheEntryType::GetMissingProperty {
            if !object.is_cacheable_for_property_absence() {
                continue;
            }
            if Some(shape) != entry.shape {
                continue;
            }
            if shape.is_dictionary() && shape.dictionary_generation() != entry.shape_dictionary_generation {
                continue;
            }
            if shape.prototype().is_some()
                && !entry
                    .prototype_chain_validity
                    .is_some_and(|validity| validity.is_valid())
            {
                continue;
            }
            return Value::UNDEFINED;
        }

        if entry.entry_type != PropertyLookupCacheEntryType::GetOwnProperty
            && entry.entry_type != PropertyLookupCacheEntryType::GetPropertyInPrototypeChain
        {
            continue;
        }

        if let Some(cached_prototype) = entry.prototype {
            if Some(shape) != entry.shape {
                continue;
            }
            if shape.is_dictionary() && shape.dictionary_generation() != entry.shape_dictionary_generation {
                continue;
            }
            if !entry
                .prototype_chain_validity
                .is_some_and(|validity| validity.is_valid())
            {
                continue;
            }
            let value = cached_prototype.get_direct(entry.property_offset);
            if value.is_accessor() {
                return Value::EMPTY;
            }
            return value;
        } else if Some(shape) == entry.shape {
            if shape.is_dictionary() && shape.dictionary_generation() != entry.shape_dictionary_generation {
                continue;
            }
            let value = object.get_direct(entry.property_offset);
            if value.is_accessor() {
                return Value::EMPTY;
            }
            return value;
        }
    }
    Value::EMPTY
}

// Fast path for GetByValue on typed arrays.
// Returns whether it stored the result in dst; if not, the caller falls to the slow path.
pub fn try_get_by_value_typed_array(vm: &Vm, values: &mut op::GetByValueValues) -> bool {
    let base = values.base;
    if !base.is_object() {
        return false;
    }

    let property = values.property;
    if !is_non_negative_int32(property) {
        return false;
    }

    let object = base.as_object();
    if !object.is_typed_array() {
        return false;
    }

    let typed_array = typed_array_of_object(&object);
    let index = property.as_i32() as u32;

    // Fast path: fixed-length typed array with cached data pointer
    let array_length = typed_array.array_length();
    if array_length.is_auto() {
        return false;
    }

    let length = array_length.length();
    if index >= length {
        values.dst = Value::UNDEFINED;
        return true;
    }

    if !is_valid_integer_index(typed_array, CanonicalIndex::new(CanonicalIndexType::Index, index)) {
        values.dst = Value::UNDEFINED;
        return true;
    }

    let buffer = typed_array.viewed_array_buffer();
    let Some(byte_index) = (index as usize)
        .checked_mul(typed_array.element_size() as usize)
        .and_then(|byte_index| byte_index.checked_add(typed_array.byte_offset() as usize))
    else {
        return false;
    };

    let element_type = match typed_array.kind() {
        Kind::Uint8Array | Kind::Uint8ClampedArray => ElementType::Uint8,
        Kind::Int8Array => ElementType::Int8,
        Kind::Uint16Array => ElementType::Uint16,
        Kind::Int16Array => ElementType::Int16,
        Kind::Uint32Array => ElementType::Uint32,
        Kind::Int32Array => ElementType::Int32,
        Kind::Float32Array => ElementType::Float32,
        Kind::Float64Array => ElementType::Float64,
        _ => return false,
    };

    values.dst = buffer.get_value(vm, byte_index, element_type, true, Order::Unordered, true);
    true
}

// Fast path for PutByValue on typed arrays.
// Returns whether it stored the value; if not, the caller falls to the slow path.
pub fn try_put_by_value_typed_array(vm: &Vm, values: &op::PutByValueValues) -> bool {
    let base = values.base;
    if !base.is_object() {
        return false;
    }

    let property = values.property;
    if !is_non_negative_int32(property) {
        return false;
    }

    let object = base.as_object();
    if !object.is_typed_array() {
        return false;
    }

    let typed_array = typed_array_of_object(&object);
    let index = property.as_i32() as u32;

    let array_length = typed_array.array_length();
    if array_length.is_auto() {
        return false;
    }

    // NB: An out-of-bounds write is not simply a no-op: TypedArraySetElement still
    //     evaluates ToNumber(value) for its side effects before discarding the store.
    //     Fall back to the slow path so those side effects happen.
    if index >= array_length.length() {
        return false;
    }

    if !is_valid_integer_index(typed_array, CanonicalIndex::new(CanonicalIndexType::Index, index)) {
        return false;
    }

    let buffer = typed_array.viewed_array_buffer();
    let Some(byte_index) = (index as usize)
        .checked_mul(typed_array.element_size() as usize)
        .and_then(|byte_index| byte_index.checked_add(typed_array.byte_offset() as usize))
    else {
        return false;
    };
    let value = values.src;

    if value.is_int32() {
        let int_value = value.as_i32();
        let (element_type, value) = match typed_array.kind() {
            Kind::Uint8Array => (ElementType::Uint8, value),
            Kind::Uint8ClampedArray => (ElementType::Uint8Clamped, Value::from_i32(int_value.clamp(0, 255))),
            Kind::Int8Array => (ElementType::Int8, value),
            Kind::Uint16Array => (ElementType::Uint16, value),
            Kind::Int16Array => (ElementType::Int16, value),
            Kind::Uint32Array => (ElementType::Uint32, value),
            Kind::Int32Array => (ElementType::Int32, value),
            _ => return false,
        };
        buffer.set_value(vm, byte_index, element_type, value, true, Order::Unordered, true);
        return true;
    }

    if value.is_double() {
        let double_value = value.as_f64();
        let element_type = match typed_array.kind() {
            Kind::Float32Array => ElementType::Float32,
            Kind::Float64Array => ElementType::Float64,
            _ => return false,
        };
        buffer.set_value(
            vm,
            byte_index,
            element_type,
            Value::from_f64(double_value),
            true,
            Order::Unordered,
            true,
        );
        return true;
    }

    false
}
