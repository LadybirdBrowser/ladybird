/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The slow paths for literals, iterators, generators and control flow, and their helpers.

use ak::{Utf16FlyString, Utf16String};

use crate::bytecode::executable::StaticPropertyLookupCacheSite;
use crate::bytecode::generator::{DisposableResourceKind, FinallyContext};
use crate::bytecode::op;
use crate::debugger::PauseReason;
use crate::interpreter::runtime_functions::{SlowPathControl, asm_try, handle_asm_exception};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::array_iterator::{
    ArrayIterator, iterated_array_like_element, iterated_array_like_length, packed_array_iteration_is_unobservable,
};
use crate::runtime::async_from_sync_iterator_prototype::create_async_from_sync_iterator;
use crate::runtime::async_generator::AsyncGenerator;
use crate::runtime::completion::{Completion, CompletionType, Must, ThrowCompletionOr, completion_type_from_bytecode};
use crate::runtime::disposable_resource_stack::{AsyncDisposeResourcesStep, DisposableResourceStack};
use crate::runtime::environment::InitializeBindingHint;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::generator_object::GeneratorObject;
use crate::runtime::iterator::{
    IteratorHint, IteratorRecord, IteratorRecordImpl, get_iterator_from_method_impl, get_iterator_impl_with_cache,
    get_iterator_values, get_sync_iterator_method, iterator_close_with_cache, iterator_hint_from_bytecode,
    iterator_next, iterator_step_value,
};
use crate::runtime::map::Map;
use crate::runtime::map_iterator::map_iteration_is_unobservable;
use crate::runtime::object::{IndexedStorageKind, IntegrityLevel, PropertyKind};
use crate::runtime::property_attributes::{Attribute, DEFAULT_ATTRIBUTES, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::realm_watchpoints;
use crate::runtime::regexp_object::RegExpObject;
use crate::runtime::set::Set;
use crate::runtime::set_iterator::set_iteration_is_unobservable;
use crate::utf16::{Utf16Display, Utf16View};

/// Throws a new error and hands it to the interpreter.
fn throw_error(
    vm: &Vm,
    pc: u32,
    kind: ErrorKind,
    error_type: ErrorType,
    arguments: &[&dyn Utf16Display],
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

/// The iterator record a slow path receives as the three registers the bytecode keeps it in, if they are not in fast
/// array mode (see get_iterator()).
fn iterator_record_from_registers(
    iterator_object: Value,
    iterator_next: Value,
    iterator_done: Value,
) -> IteratorRecordImpl {
    assert!(iterator_object.is_object());
    IteratorRecordImpl::new(
        Some(iterator_object.as_object()),
        iterator_next,
        iterator_done.as_bool(),
    )
}

/// The primitive values a NewPrimitiveArray instruction carries after its fixed fields.
fn primitive_array_elements(instruction: &op::NewPrimitiveArray) -> &[Value] {
    instruction.elements()
}

pub fn debugger_check_breakpoint(vm: &Vm, pc: u32) {
    // NB: The dispatch table is chosen when entering the interpreter, so we keep getting called
    //     for the rest of the frame even if the host detaches its debugger in the meantime.
    let Some(debugger) = vm.debugger() else {
        return;
    };

    // NB: Debugger callbacks must not inspect slots before Enter initializes them.
    if !vm.running_execution_context_ref().frame_initialized.get() {
        return;
    }

    let executable = vm.current_executable();
    debugger.register_executable(vm, executable);
    let reason = if debugger.should_pause_on_next_bytecode_execution(&executable, pc) {
        Some(PauseReason::Entry)
    } else if executable.has_debugger_breakpoint_at(pc) {
        Some(PauseReason::Breakpoint)
    } else if debugger.should_pause_for_step(vm, &executable, pc) {
        Some(PauseReason::Step)
    } else {
        None
    };

    let did_pause = reason.is_some_and(|reason| debugger.pause_execution(vm, executable, pc, reason, None, false));
    debugger.set_did_pause_before_current_instruction(did_pause);
}

pub fn fallback_handler(_pc: u32) -> SlowPathControl {
    // NB: Every bytecode opcode has a DSL handler, so this should never run.
    unreachable!("every bytecode opcode has a handler")
}

/// The array of an array literal with `elements`, where the empty value is a hole.
fn create_array_literal(vm: &Vm, elements: &[Value]) -> Gc<Array> {
    // OPTIMIZATION: Without holes, the elements become the packed indexed storage of the array in one step.
    if !elements.iter().any(|element| element.is_empty()) {
        return Array::create_from(vm, current_realm(vm), elements);
    }
    let array = Array::create(vm, current_realm(vm), elements.len() as u64, None).must();
    for (index, element) in elements.iter().enumerate() {
        array.indexed_put(index as u32, *element, DEFAULT_ATTRIBUTES);
    }
    array
}

pub fn new_array(
    vm: &Vm,
    pc: u32,
    instruction: &op::NewArray,
    values: &mut op::NewArrayValues,
    elements: &[Value],
) -> SlowPathControl {
    values.dst = Value::from_object(create_array_literal(vm, elements));
    SlowPathControl::continue_at(pc + instruction.length())
}

pub fn new_primitive_array(
    vm: &Vm,
    pc: u32,
    instruction: &op::NewPrimitiveArray,
    values: &mut op::NewPrimitiveArrayValues,
) -> SlowPathControl {
    values.dst = Value::from_object(create_array_literal(vm, primitive_array_elements(instruction)));
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
        let rhs_set = if rhs.is_object() {
            rhs.as_object().downcast::<Set>()
        } else {
            None
        };
        let rhs_map = if rhs.is_object() {
            rhs.as_object().downcast::<Map>()
        } else {
            None
        };
        let mut iterator_record = None;

        if (rhs_array.is_some() || rhs_set.is_some() || rhs_map.is_some())
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
            let realm = current_realm(vm);
            if let Some(rhs_array) = rhs_array
                && packed_array_iteration_is_unobservable(vm, realm, &rhs_array, iterator_method)
                && rhs_array.indexed_packed_element_count() <= u32::MAX - lhs_size
            {
                lhs_array.indexed_append_packed_elements_of(&rhs_array);
                return SlowPathControl::continue_at(pc + op::ArrayAppend::LENGTH);
            }

            // OPTIMIZATION: Iterating a Set or Map with its original iteration functions cannot be observed, and no
            //               user code runs while we append, so the collection storage can be read directly.
            if let Some(rhs_set) = rhs_set
                && set_iteration_is_unobservable(vm, current_realm(vm), iterator_method)
                && rhs_set.set_size() <= (u32::MAX - lhs_size) as usize
            {
                let mut index = lhs_size;
                rhs_set.for_each_value(|value| {
                    lhs_array.indexed_put(index, value, DEFAULT_ATTRIBUTES);
                    index += 1;
                });
                return SlowPathControl::continue_at(pc + op::ArrayAppend::LENGTH);
            }
            if let Some(rhs_map) = rhs_map
                && map_iteration_is_unobservable(vm, current_realm(vm), iterator_method)
                && rhs_map.map_size() <= (u32::MAX - lhs_size) as usize
            {
                // NB: Creating the entry arrays can trigger garbage collection, which does not modify maps.
                let realm = current_realm(vm);
                let mut index = lhs_size;
                rhs_map.for_each_entry(|key, value| {
                    let entry = Array::create_from(vm, realm, &[key, value]);
                    lhs_array.indexed_put(index, Value::from_object(entry), DEFAULT_ATTRIBUTES);
                    index += 1;
                });
                return SlowPathControl::continue_at(pc + op::ArrayAppend::LENGTH);
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
                // NB: The index is truncated to the u32 that indexed_put() takes.
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

pub fn new_regexp(vm: &Vm, pc: u32, instruction: &op::NewRegExp, values: &mut op::NewRegExpValues) -> SlowPathControl {
    let realm = current_realm(vm);
    let executable = vm.current_executable();
    let regexp_object = RegExpObject::create_with_pattern_and_flags(
        vm,
        realm,
        Utf16String::from(executable.get_string(instruction.source_index)),
        Utf16String::from(executable.get_string(instruction.flags_index)),
    );
    regexp_object.set_realm(realm);
    regexp_object.set_legacy_features_enabled(true);
    values.dst = Value::from_object(regexp_object);
    SlowPathControl::continue_at(pc + op::NewRegExp::LENGTH)
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

// NB: In fast array mode, GetIterator makes no iterator for an array whose iteration nothing can observe. The
//     iterator register holds the index of the next element as a number, and the next register holds the array.
//     The done register tells when the iteration is done. IteratorNextUnpack, IteratorClose and IteratorToArray step
//     it like an array iterator. Other iterator records always hold an object in the iterator register.

/// The index of the next element in fast array mode, if the iterator register is in that mode.
fn fast_array_mode_index(iterator_object: Value) -> Option<f64> {
    iterator_object.is_number().then(|| iterator_object.as_f64())
}

/// The array of an iteration in fast array mode, or `None` if GetIterator must make an iterator for `iterable` whose
/// iterator method is `method`. The array iterator that the method would make has the builtin next method, and only
/// that method reads the array.
fn fast_array_mode_array(vm: &Vm, iterable: Value, method: Gc<FunctionObject>) -> Option<Gc<Object>> {
    if !iterable.is_object() {
        return None;
    }
    let realm = current_realm(vm);
    if method != realm.array_prototype_values_function()
        || !realm_watchpoints::array_iterators_have_builtin_next(vm, realm)
    {
        return None;
    }
    let array = iterable.as_object();
    array.downcast::<Array>().is_some().then_some(array)
}

/// The next step of an iteration in fast array mode at `index`, like %ArrayIteratorPrototype%.next ( ): the element
/// and the index after it, or `None` once the iteration is done.
fn fast_array_mode_step(vm: &Vm, array: &Object, index: f64) -> ThrowCompletionOr<Option<(Value, Value)>> {
    let index = index as u64;
    if index >= iterated_array_like_length(vm, array)? {
        return Ok(None);
    }
    let element = iterated_array_like_element(vm, array, index)?;
    Ok(Some((element, Value::from_f64((index + 1) as f64))))
}

pub fn get_iterator(
    vm: &Vm,
    pc: u32,
    instruction: &op::GetIterator,
    values: &mut op::GetIteratorValues,
) -> SlowPathControl {
    let executable = vm.current_executable();
    let cache = executable.property_lookup_cache(instruction.cache as usize);
    let hint = iterator_hint_from_bytecode(instruction.hint);
    if hint == IteratorHint::Async {
        let iterator_record = asm_try!(vm, pc, get_iterator_impl_with_cache(vm, values.iterable, hint, cache));
        values.dst_iterator_object = Value::from_object(iterator_record.iterator());
        values.dst_iterator_next = iterator_record.next_method();
        values.dst_iterator_done = Value::from_bool(iterator_record.done());
        return SlowPathControl::continue_at(pc + op::GetIterator::LENGTH);
    }

    let method = asm_try!(vm, pc, get_sync_iterator_method(vm, values.iterable, cache));

    // OPTIMIZATION: Calling %Array.prototype.values% and getting the next method of the array iterator it makes
    //               cannot be observed, so fast array mode skips both.
    if instruction.fast_array_mode_allowed
        && let Some(array) = fast_array_mode_array(vm, values.iterable, method)
    {
        values.dst_iterator_object = Value::from_i32(0);
        values.dst_iterator_next = Value::from_object(array);
        values.dst_iterator_done = Value::FALSE;
        return SlowPathControl::continue_at(pc + op::GetIterator::LENGTH);
    }

    let iterator_record = asm_try!(vm, pc, get_iterator_from_method_impl(vm, values.iterable, method));
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
    let completion = Completion::new(
        completion_type_from_bytecode(instruction.completion_type),
        values.completion_value,
    );
    let executable = vm.current_executable();
    let cache = executable.property_lookup_cache(instruction.cache as usize);

    let iterator_record = if let Some(index) = fast_array_mode_index(values.iterator_object) {
        // NB: Without a return method, closing an array iterator does nothing.
        let realm = current_realm(vm);
        if realm_watchpoints::array_iterators_have_no_return(vm, realm) {
            return SlowPathControl::continue_at(pc + op::IteratorClose::LENGTH);
        }

        // A return method appeared during the iteration. It gets the array iterator that GetIterator would have
        // made, at the same position.
        let iterator = ArrayIterator::create(vm, realm, values.iterator_next, PropertyKind::Value);
        if values.iterator_done.as_bool() {
            iterator.array.set(Value::UNDEFINED);
        } else {
            iterator.index.set(index as u64);
        }
        IteratorRecordImpl::new(
            Some(iterator.upcast()),
            Value::UNDEFINED,
            values.iterator_done.as_bool(),
        )
    } else {
        iterator_record_from_registers(values.iterator_object, values.iterator_next, values.iterator_done)
    };

    asm_try!(
        vm,
        pc,
        iterator_close_with_cache(vm, &iterator_record, completion, cache).into_throw_completion_or()
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
    if let Some(index) = fast_array_mode_index(values.iterator_object) {
        let step = if values.iterator_done.as_bool() {
            Ok(None)
        } else {
            fast_array_mode_step(vm, &values.iterator_next.as_object(), index)
        };
        if !matches!(step, Ok(Some(_))) {
            values.iterator_done = Value::TRUE;
        }
        if let Some((element, next_index)) = asm_try!(vm, pc, step) {
            values.dst_value = element;
            values.dst_done = Value::FALSE;
            values.iterator_object = next_index;
        } else {
            values.dst_value = Value::UNDEFINED;
            values.dst_done = Value::TRUE;
        }
        return SlowPathControl::continue_at(pc + op::IteratorNextUnpack::LENGTH);
    }

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
    let array = Array::create(vm, current_realm(vm), 0, None).must();
    let mut index: u64 = 0;

    if let Some(mut iterated_index) = fast_array_mode_index(values.iterator_object) {
        let was_done = values.iterator_done_property.as_bool();
        // NB: Every step either throws or reaches the end, which both make the iteration done.
        values.iterator_done_property = Value::TRUE;
        let iterated_array = values.iterator_next_method.as_object();
        while !was_done
            && let Some((element, next_index)) =
                asm_try!(vm, pc, fast_array_mode_step(vm, &iterated_array, iterated_index))
        {
            array
                .create_data_property_or_throw(vm, &PropertyKey::from_number(index), element)
                .must();
            index += 1;
            iterated_index = next_index.as_f64();
        }
        values.dst = Value::from_object(array);
        return SlowPathControl::continue_at(pc + op::IteratorToArray::LENGTH);
    }

    let iterator_record = iterator_record_from_registers(
        values.iterator_object,
        values.iterator_next_method,
        values.iterator_done_property,
    );

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

pub fn new_disposable_resource_stack(
    vm: &Vm,
    pc: u32,
    values: &mut op::NewDisposableResourceStackValues,
) -> SlowPathControl {
    values.dst = DisposableResourceStack::to_value(DisposableResourceStack::create(vm));
    SlowPathControl::continue_at(pc + op::NewDisposableResourceStack::LENGTH)
}

pub fn add_disposable_resource(
    vm: &Vm,
    pc: u32,
    instruction: &op::AddDisposableResource,
    values: &mut op::AddDisposableResourceValues,
) -> SlowPathControl {
    let hint = if instruction.hint == DisposableResourceKind::AsyncDispose as u32 {
        InitializeBindingHint::AsyncDispose
    } else {
        InitializeBindingHint::SyncDispose
    };
    let stack = DisposableResourceStack::from_value(values.stack);
    asm_try!(vm, pc, stack.add_disposable_resource(vm, values.value, hint));
    SlowPathControl::continue_at(pc + op::AddDisposableResource::LENGTH)
}

/// The completion of a scope as the bytecode keeps it while it disposes of the resources of the scope: a throw
/// completion if its type is the throw type of a finally context, and a normal one otherwise. The resources are
/// disposed of the same way for every other type.
fn completion_of_scope(completion_type: Value, completion_value: Value) -> Completion {
    if completion_type.is_int32() && completion_type.as_i32() == FinallyContext::THROW {
        Completion::new(CompletionType::Throw, completion_value)
    } else {
        Completion::normal(Value::UNDEFINED)
    }
}

pub fn dispose_resources(vm: &Vm, pc: u32, values: &mut op::DisposeResourcesValues) -> SlowPathControl {
    let stack = DisposableResourceStack::from_value(values.stack);
    let completion = completion_of_scope(values.completion_type, values.completion_value);
    let result = stack.dispose_resources(vm, completion);
    if result.is_error() {
        return handle_asm_exception(vm, pc, result.value());
    }
    SlowPathControl::continue_at(pc + op::DisposeResources::LENGTH)
}

pub fn async_dispose_resources_step(
    vm: &Vm,
    pc: u32,
    values: &mut op::AsyncDisposeResourcesStepValues,
) -> SlowPathControl {
    let stack = DisposableResourceStack::from_value(values.stack);
    let completion = completion_of_scope(values.completion_type, values.completion_value);
    let await_completion = if values.await_completion_type.is_int32()
        && values.await_completion_type.as_i32() == CompletionType::Throw as i32
    {
        Completion::new(CompletionType::Throw, values.await_completion_value)
    } else {
        Completion::normal(values.await_completion_value)
    };
    match asm_try!(
        vm,
        pc,
        stack.async_dispose_resources_step(vm, completion, await_completion)
    ) {
        AsyncDisposeResourcesStep::Await(value) => {
            values.done = Value::from_bool(false);
            values.awaited_value = value;
        }
        AsyncDisposeResourcesStep::Done => {
            values.done = Value::from_bool(true);
            values.awaited_value = Value::UNDEFINED;
        }
    }
    SlowPathControl::continue_at(pc + op::AsyncDisposeResourcesStep::LENGTH)
}

pub fn get_completion_fields(_vm: &Vm, pc: u32, values: &mut op::GetCompletionFieldsValues) -> SlowPathControl {
    let completion_source = values.completion.as_object();
    if let Some(generator) = completion_source.downcast::<GeneratorObject>() {
        values.value_dst = generator.pending_completion_value();
        values.type_dst = Value::from_i32(generator.pending_completion_type() as i32);
        return SlowPathControl::continue_at(pc + op::GetCompletionFields::LENGTH);
    }

    let async_generator = completion_source
        .downcast::<AsyncGenerator>()
        .expect("a completion source is a generator or an async generator");
    values.value_dst = async_generator.pending_completion_value();
    values.type_dst = Value::from_i32(async_generator.pending_completion_type() as i32);
    SlowPathControl::continue_at(pc + op::GetCompletionFields::LENGTH)
}

pub fn set_completion_type(
    _vm: &Vm,
    pc: u32,
    instruction: &op::SetCompletionType,
    values: &mut op::SetCompletionTypeValues,
) -> SlowPathControl {
    let completion_source = values.completion.as_object();
    if let Some(generator) = completion_source.downcast::<GeneratorObject>() {
        generator.set_pending_completion_type(completion_type_from_bytecode(instruction.completion_type));
        return SlowPathControl::continue_at(pc + op::SetCompletionType::LENGTH);
    }

    completion_source
        .downcast::<AsyncGenerator>()
        .expect("a completion source is a generator or an async generator")
        .set_pending_completion_type(completion_type_from_bytecode(instruction.completion_type));
    SlowPathControl::continue_at(pc + op::SetCompletionType::LENGTH)
}

pub fn debugger(vm: &Vm, pc: u32) -> SlowPathControl {
    // NB: Don't pause twice if the debugger trampoline already paused before this instruction.
    if let Some(debugger) = vm.debugger()
        && !debugger.did_pause_before_current_instruction()
    {
        debugger.pause_execution(
            vm,
            vm.current_executable(),
            pc,
            PauseReason::DebuggerStatement,
            None,
            false,
        );
    }
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

pub fn throw_not_a_function(vm: &Vm, pc: u32, values: &op::ThrowNotAFunctionValues) -> SlowPathControl {
    throw_error(vm, pc, ErrorKind::TypeError, ErrorType::NotAFunction, &[&values.src])
}

pub fn get_argument_count(vm: &Vm, pc: u32, values: &mut op::GetArgumentCountValues) -> SlowPathControl {
    let passed_argument_count = vm.running_execution_context_ref().passed_argument_count.get();
    values.dst = Value::from_f64(f64::from(passed_argument_count));
    SlowPathControl::continue_at(pc + op::GetArgumentCount::LENGTH)
}

pub fn throw_const_assignment(vm: &Vm, pc: u32) -> SlowPathControl {
    throw_error(vm, pc, ErrorKind::TypeError, ErrorType::InvalidAssignToConst, &[])
}

pub fn r#await(vm: &Vm, instruction: &op::Await, values: &op::AwaitValues) -> SlowPathControl {
    let yielded_value = if values.argument.is_empty() {
        Value::UNDEFINED
    } else {
        values.argument
    };
    let context = running_execution_context(vm);
    context.yield_continuation.set(instruction.continuation_label.0);
    context.yield_is_await.set(true);
    context.yield_value_is_iterator_result.set(false);
    vm.do_return(yielded_value);
    SlowPathControl::EXIT
}

pub fn r#yield(vm: &Vm, instruction: &op::Yield, values: &op::YieldValues) -> SlowPathControl {
    let yielded_value = if values.value.is_empty() {
        Value::UNDEFINED
    } else {
        values.value
    };
    let context = running_execution_context(vm);
    match instruction.continuation_label.get() {
        Some(continuation_label) => context.yield_continuation.set(continuation_label.0),
        None => context.yield_continuation.set(ExecutionContext::NO_YIELD_CONTINUATION),
    }
    context.yield_is_await.set(false);
    context.yield_value_is_iterator_result.set(false);
    vm.do_return(yielded_value);
    SlowPathControl::EXIT
}

pub fn yield_iterator_result(
    vm: &Vm,
    instruction: &op::YieldIteratorResult,
    values: &op::YieldIteratorResultValues,
) -> SlowPathControl {
    let yielded_value = if values.value.is_empty() {
        Value::UNDEFINED
    } else {
        values.value
    };
    let context = running_execution_context(vm);
    context.yield_continuation.set(instruction.continuation_label.0);
    context.yield_is_await.set(false);
    context.yield_value_is_iterator_result.set(true);
    vm.do_return(yielded_value);
    SlowPathControl::EXIT
}

fn running_execution_context(vm: &Vm) -> &ExecutionContext {
    vm.running_execution_context_if_any()
        .expect("a generator's frame is running")
}
