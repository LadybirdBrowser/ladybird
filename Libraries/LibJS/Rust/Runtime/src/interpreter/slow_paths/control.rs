/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The slow paths for literals, iterators, generators and control flow, and their helpers.

use ak::Utf16FlyString;

use crate::bytecode::executable::StaticPropertyLookupCacheSite;
use crate::bytecode::op;
use crate::bytecode::property_access::get_own_property_without_side_effects;
use crate::interpreter::runtime_functions::{
    SlowPathControl, asm_try, handle_asm_exception, unimplemented_runtime_function,
};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::async_from_sync_iterator_prototype::create_async_from_sync_iterator;
use crate::runtime::completion::{Completion, Must, completion_type_from_bytecode};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::iterator::{
    IteratorRecord, IteratorRecordImpl, get_iterator_from_method_impl, get_iterator_impl, get_iterator_values,
    iterator_close, iterator_hint_from_bytecode, iterator_next, iterator_step_value,
};
use crate::runtime::object::{IndexedStorageKind, IntegrityLevel};
use crate::runtime::property_attributes::{Attribute, DEFAULT_ATTRIBUTES, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::utf16::Utf16View;

/// Throws a new error and hands it to the interpreter.
fn throw_error(
    vm: &Vm,
    pc: u32,
    kind: ErrorKind,
    error_type: ErrorType,
    arguments: &[&dyn core::fmt::Display],
) -> SlowPathControl {
    match vm.throw_completion::<()>(kind, error_type, arguments) {
        Err(throw) => handle_asm_exception(vm, pc, throw.value()),
        Ok(()) => unreachable!("throw_completion always throws"),
    }
}

fn current_realm(vm: &Vm) -> Gc<Realm> {
    vm.current_realm().expect("slow paths run with a current realm")
}

fn create_type_error(vm: &Vm, realm: Gc<Realm>, message: &Utf16FlyString) -> Gc<Object> {
    ErrorKind::TypeError
        .create(vm, realm, Utf16View::of_fly_string(message).to_utf16_string())
        .upcast()
}

fn create_reference_error(vm: &Vm, realm: Gc<Realm>, message: &Utf16FlyString) -> Gc<Object> {
    ErrorKind::ReferenceError
        .create(vm, realm, Utf16View::of_fly_string(message).to_utf16_string())
        .upcast()
}

/// The iterator record a slow path receives as the three registers the bytecode keeps it in.
fn iterator_record_from_registers(
    iterator_object: Value,
    iterator_next: Value,
    iterator_done: Value,
) -> IteratorRecordImpl {
    IteratorRecordImpl::new(
        Some(iterator_object.as_object()),
        iterator_next,
        iterator_done.as_bool(),
    )
}

/// The primitive values a NewPrimitiveArray instruction carries after its fixed fields.
fn primitive_array_elements(instruction: &op::NewPrimitiveArray) -> &[Value] {
    // SAFETY: The bytecode holds element_count values right after the fixed fields of the instruction, within the
    // length it records, and it does not change while the executable that holds it is alive.
    unsafe { core::slice::from_raw_parts(instruction.elements.as_ptr(), instruction.element_count as usize) }
}

pub fn debugger_check_breakpoint(_vm: &Vm, _pc: u32) {
    // NB: The Rust runtime has no debugger, and C++ returns right away without one.
}

pub fn fallback_handler(_pc: u32) -> SlowPathControl {
    // NB: Every bytecode opcode has a DSL handler, so this should never run.
    unreachable!("every bytecode opcode has a handler")
}

pub fn new_array(
    vm: &Vm,
    pc: u32,
    instruction: &op::NewArray,
    values: &mut op::NewArrayValues,
    elements: &[Value],
) -> SlowPathControl {
    let array = Array::create(vm, current_realm(vm), u64::from(instruction.element_count), None).must();
    for (index, element) in elements.iter().enumerate() {
        array.indexed_put(index as u32, *element, DEFAULT_ATTRIBUTES);
    }
    values.dst = Value::from_object(array);
    SlowPathControl::continue_at(pc + instruction.length())
}

pub fn new_primitive_array(
    vm: &Vm,
    pc: u32,
    instruction: &op::NewPrimitiveArray,
    values: &mut op::NewPrimitiveArrayValues,
) -> SlowPathControl {
    let array = Array::create(vm, current_realm(vm), u64::from(instruction.element_count), None).must();
    for (index, element) in primitive_array_elements(instruction).iter().enumerate() {
        array.indexed_put(index as u32, *element, DEFAULT_ATTRIBUTES);
    }
    values.dst = Value::from_object(array);
    SlowPathControl::continue_at(pc + instruction.length())
}

pub fn new_array_with_length(vm: &Vm, pc: u32, values: &mut op::NewArrayWithLengthValues) -> SlowPathControl {
    let length = values.array_length.as_f64() as u64;
    let array = asm_try!(vm, pc, Array::create(vm, current_realm(vm), length, None));
    values.dst = Value::from_object(array);
    SlowPathControl::continue_at(pc + op::NewArrayWithLength::LENGTH)
}

pub fn array_append(
    vm: &Vm,
    pc: u32,
    instruction: &op::ArrayAppend,
    values: &mut op::ArrayAppendValues,
) -> SlowPathControl {
    let rhs = values.src;
    let lhs_array = values.dst.as_object();
    debug_assert!(lhs_array.is_array_exotic_object());
    let lhs_size = lhs_array.indexed_array_like_size();

    if instruction.is_spread {
        let rhs_array = if rhs.is_object() {
            rhs.as_object().downcast::<Array>()
        } else {
            None
        };
        // NB: The C++ runtime also appends Set and Map objects in bulk here, which come with the builtins that
        //     create them.
        let mut iterator_record = None;

        if rhs_array.is_some()
            && matches!(
                lhs_array.indexed_storage_kind(),
                IndexedStorageKind::None | IndexedStorageKind::Packed
            )
        {
            let iterator_method = asm_try!(
                vm,
                pc,
                rhs.get_method_with_cache(
                    vm,
                    &PropertyKey::from(vm.well_known_symbols().iterator),
                    vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::ArrayAppendIteratorMethod),
                )
            );
            let Some(iterator_method) = iterator_method else {
                return throw_error(vm, pc, ErrorKind::TypeError, ErrorType::NotIterable, &[&rhs]);
            };

            // OPTIMIZATION: The original array iterator has no observable side effects, so a packed
            //               array can be appended in bulk if its next method is also unchanged.
            let original_iterator_method = current_realm(vm).array_prototype_values_function();
            if let Some(rhs_array) = rhs_array
                && iterator_method == original_iterator_method
                && rhs_array.is_simple_packed_array()
            {
                let iterator_prototype = current_realm(vm).array_iterator_prototype();

                // NB: Inspect the intrinsic prototype's own property without invoking it. Using get()
                //     here would call an accessor with the prototype as its receiver, whereas the
                //     iterator protocol calls it with the newly created iterator as its receiver.
                //     Accessors and replacement methods therefore take the generic path below.
                let next_method = get_own_property_without_side_effects(
                    &iterator_prototype,
                    &vm.names.next,
                    vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::ArrayAppendNextMethod),
                );
                if next_method.is_function()
                    && next_method.as_function().as_native_function().is_some()
                    && next_method.as_function().is_array_prototype_next_builtin()
                    && rhs_array.indexed_packed_element_count() <= u32::MAX - lhs_size
                {
                    lhs_array.indexed_append_packed_elements_of(&rhs_array);
                    return SlowPathControl::continue_at(pc + op::ArrayAppend::LENGTH);
                }
            }

            iterator_record = Some(asm_try!(
                vm,
                pc,
                get_iterator_from_method_impl(vm, rhs, iterator_method)
            ));
        }

        let mut index = u64::from(lhs_size);
        if let Some(iterator_record) = iterator_record {
            loop {
                let iterator_value = asm_try!(vm, pc, iterator_step_value(vm, &iterator_record));
                let Some(iterator_value) = iterator_value else {
                    break;
                };
                // NB: The C++ runtime truncates the size_t index to the u32 indexed_put() takes.
                lhs_array.indexed_put(index as u32, iterator_value, DEFAULT_ATTRIBUTES);
                index += 1;
            }
            return SlowPathControl::continue_at(pc + op::ArrayAppend::LENGTH);
        }

        let result = get_iterator_values(vm, rhs, |iterator_value| {
            lhs_array.indexed_put(index as u32, iterator_value, DEFAULT_ATTRIBUTES);
            index += 1;
            None
        });
        if result.is_error() {
            return handle_asm_exception(vm, pc, result.value());
        }
    } else {
        lhs_array.indexed_put(lhs_size, rhs, DEFAULT_ATTRIBUTES);
    }

    SlowPathControl::continue_at(pc + op::ArrayAppend::LENGTH)
}

pub fn get_template_object(
    vm: &Vm,
    pc: u32,
    instruction: &op::GetTemplateObject,
    values: &mut op::GetTemplateObjectValues,
    strings: &[Value],
) -> SlowPathControl {
    let cache = vm.current_executable().template_object_cache(instruction.cache);

    if let Some(cached_template_object) = cache.cached_template_object() {
        values.dst = Value::from_object(cached_template_object);
        return SlowPathControl::continue_at(pc + instruction.length());
    }

    let realm = current_realm(vm);
    let count = instruction.strings_count / 2;
    let template_object = Array::create(vm, realm, u64::from(count), None).must();
    let raw_object = Array::create(vm, realm, u64::from(count), None).must();

    let enumerable = PropertyAttributes::new(Attribute::ENUMERABLE);
    for index in 0..count {
        template_object.indexed_put(index, strings[index as usize], enumerable);
        raw_object.indexed_put(index, strings[(count + index) as usize], enumerable);
    }

    raw_object.set_integrity_level(vm, IntegrityLevel::Frozen).must();
    template_object.define_direct_property(
        vm,
        &vm.names.raw,
        Value::from_object(raw_object),
        PropertyAttributes::default(),
    );
    template_object.set_integrity_level(vm, IntegrityLevel::Frozen).must();

    cache.set_cached_template_object(template_object);
    values.dst = Value::from_object(template_object);
    SlowPathControl::continue_at(pc + instruction.length())
}

pub fn new_regexp(vm: &Vm, pc: u32, instruction: &op::NewRegExp) -> SlowPathControl {
    let executable = vm.current_executable();
    let source = Utf16View::of_fly_string(executable.get_string(instruction.source_index)).to_utf8();
    let flags = Utf16View::of_fly_string(executable.get_string(instruction.flags_index)).to_utf8();
    unimplemented_runtime_function(
        &format!(
            "RegExpObject::create for the regular expression literal /{source}/{flags}, which needs the regex engine \
             and %RegExp.prototype%"
        ),
        pc,
    )
}

pub fn new_reference_error(
    vm: &Vm,
    pc: u32,
    instruction: &op::NewReferenceError,
    values: &mut op::NewReferenceErrorValues,
) -> SlowPathControl {
    let realm = current_realm(vm);
    let executable = vm.current_executable();
    values.dst = Value::from_object(create_reference_error(
        vm,
        realm,
        executable.get_string(instruction.error_string),
    ));
    SlowPathControl::continue_at(pc + op::NewReferenceError::LENGTH)
}

pub fn new_type_error(
    vm: &Vm,
    pc: u32,
    instruction: &op::NewTypeError,
    values: &mut op::NewTypeErrorValues,
) -> SlowPathControl {
    let realm = current_realm(vm);
    let executable = vm.current_executable();
    values.dst = Value::from_object(create_type_error(
        vm,
        realm,
        executable.get_string(instruction.error_string),
    ));
    SlowPathControl::continue_at(pc + op::NewTypeError::LENGTH)
}

pub fn get_iterator(
    vm: &Vm,
    pc: u32,
    instruction: &op::GetIterator,
    values: &mut op::GetIteratorValues,
) -> SlowPathControl {
    let iterator_record = asm_try!(
        vm,
        pc,
        get_iterator_impl(vm, values.iterable, iterator_hint_from_bytecode(instruction.hint))
    );
    values.dst_iterator_object = Value::from_object(iterator_record.iterator());
    values.dst_iterator_next = iterator_record.next_method();
    values.dst_iterator_done = Value::from_bool(iterator_record.done());
    SlowPathControl::continue_at(pc + op::GetIterator::LENGTH)
}

pub fn iterator_close_slow_path(
    vm: &Vm,
    pc: u32,
    instruction: &op::IteratorClose,
    values: &mut op::IteratorCloseValues,
) -> SlowPathControl {
    let iterator_record =
        iterator_record_from_registers(values.iterator_object, values.iterator_next, values.iterator_done);

    let completion = Completion::new(
        completion_type_from_bytecode(instruction.completion_type),
        values.completion_value,
    );
    asm_try!(
        vm,
        pc,
        iterator_close(vm, &iterator_record, completion).into_throw_completion_or()
    );
    SlowPathControl::continue_at(pc + op::IteratorClose::LENGTH)
}

pub fn iterator_next_slow_path(vm: &Vm, pc: u32, values: &mut op::IteratorNextValues) -> SlowPathControl {
    let iterator_record =
        iterator_record_from_registers(values.iterator_object, values.iterator_next, values.iterator_done);
    let result = iterator_next(vm, &iterator_record, None);
    if iterator_record.done() {
        values.iterator_done = Value::TRUE;
    }
    values.dst = Value::from_object(asm_try!(vm, pc, result));
    SlowPathControl::continue_at(pc + op::IteratorNext::LENGTH)
}

pub fn iterator_next_unpack(vm: &Vm, pc: u32, values: &mut op::IteratorNextUnpackValues) -> SlowPathControl {
    let iterator_record =
        iterator_record_from_registers(values.iterator_object, values.iterator_next, values.iterator_done);
    let iteration_result_or_error = iterator_step_value(vm, &iterator_record);
    if iterator_record.done() {
        values.iterator_done = Value::TRUE;
    }

    if let Some(iteration_result) = asm_try!(vm, pc, iteration_result_or_error) {
        values.dst_value = iteration_result;
        values.dst_done = Value::FALSE;
    } else {
        values.dst_value = Value::UNDEFINED;
        values.dst_done = Value::TRUE;
    }

    SlowPathControl::continue_at(pc + op::IteratorNextUnpack::LENGTH)
}

pub fn iterator_to_array(vm: &Vm, pc: u32, values: &mut op::IteratorToArrayValues) -> SlowPathControl {
    let iterator_record = iterator_record_from_registers(
        values.iterator_object,
        values.iterator_next_method,
        values.iterator_done_property,
    );

    let array = Array::create(vm, current_realm(vm), 0, None).must();
    let mut index: u64 = 0;
    loop {
        let value_or_error = iterator_step_value(vm, &iterator_record);
        if iterator_record.done() {
            values.iterator_done_property = Value::TRUE;
        }
        let value = asm_try!(vm, pc, value_or_error);
        let Some(value) = value else {
            values.dst = Value::from_object(array);
            return SlowPathControl::continue_at(pc + op::IteratorToArray::LENGTH);
        };

        array
            .create_data_property_or_throw(vm, &PropertyKey::from_number(index), value)
            .must();
        index += 1;
    }
}

pub fn create_async_from_sync_iterator_slow_path(
    vm: &Vm,
    pc: u32,
    values: &mut op::CreateAsyncFromSyncIteratorValues,
) -> SlowPathControl {
    let iterator = values.iterator.as_object();
    let next_method = values.next_method;
    let done = values.done.as_bool();

    let iterator_record = IteratorRecord::create(vm, Some(iterator), next_method, done);
    let async_from_sync_iterator = create_async_from_sync_iterator(vm, iterator_record);

    let realm = current_realm(vm);
    let iterator_object = Object::create(vm, realm, None);
    iterator_object.define_direct_property(
        vm,
        &vm.names.iterator,
        Value::from_object(async_from_sync_iterator.iterator()),
        DEFAULT_ATTRIBUTES,
    );
    iterator_object.define_direct_property(
        vm,
        &vm.names.nextMethod,
        async_from_sync_iterator.next_method(),
        DEFAULT_ATTRIBUTES,
    );
    iterator_object.define_direct_property(
        vm,
        &vm.names.done,
        Value::from_bool(async_from_sync_iterator.done()),
        DEFAULT_ATTRIBUTES,
    );

    values.dst = Value::from_object(iterator_object);
    SlowPathControl::continue_at(pc + op::CreateAsyncFromSyncIterator::LENGTH)
}

pub fn get_completion_fields(_vm: &Vm, pc: u32, values: &mut op::GetCompletionFieldsValues) -> SlowPathControl {
    let _completion_source = values.completion.as_object();
    unimplemented_runtime_function(
        "GetCompletionFields, which reads the pending completion of a GeneratorObject or AsyncGenerator, which come \
         with generators",
        pc,
    )
}

pub fn set_completion_type(_vm: &Vm, pc: u32, values: &mut op::SetCompletionTypeValues) -> SlowPathControl {
    let _completion_source = values.completion.as_object();
    unimplemented_runtime_function(
        "SetCompletionType, which sets the pending completion of a GeneratorObject or AsyncGenerator, which come \
         with generators",
        pc,
    )
}

pub fn debugger(pc: u32) -> SlowPathControl {
    // NB: The Rust runtime has no debugger to pause in, and C++ continues past the statement without one.
    SlowPathControl::continue_at(pc + op::Debugger::LENGTH)
}

pub fn throw_if_tdz(vm: &Vm, pc: u32, values: &op::ThrowIfTDZValues) -> SlowPathControl {
    let value = values.src;
    if value.is_empty() {
        return throw_error(
            vm,
            pc,
            ErrorKind::ReferenceError,
            ErrorType::BindingNotInitialized,
            &[&value],
        );
    }
    SlowPathControl::continue_at(pc + op::ThrowIfTDZ::LENGTH)
}

pub fn throw_if_not_object(vm: &Vm, pc: u32, values: &op::ThrowIfNotObjectValues) -> SlowPathControl {
    let src = values.src;
    if !src.is_object() {
        return throw_error(vm, pc, ErrorKind::TypeError, ErrorType::NotAnObject, &[&src]);
    }
    SlowPathControl::continue_at(pc + op::ThrowIfNotObject::LENGTH)
}

pub fn throw_if_nullish(vm: &Vm, pc: u32, values: &op::ThrowIfNullishValues) -> SlowPathControl {
    let value = values.src;
    if value.is_nullish() {
        return throw_error(vm, pc, ErrorKind::TypeError, ErrorType::NotObjectCoercible, &[&value]);
    }
    SlowPathControl::continue_at(pc + op::ThrowIfNullish::LENGTH)
}

pub fn throw_const_assignment(vm: &Vm, pc: u32) -> SlowPathControl {
    throw_error(vm, pc, ErrorKind::TypeError, ErrorType::InvalidAssignToConst, &[])
}
