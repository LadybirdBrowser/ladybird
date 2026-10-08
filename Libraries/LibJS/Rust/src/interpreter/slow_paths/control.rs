/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The slow paths for literals, iterators, generators and control flow, and their helpers.

use ak::{Utf16FlyString, Utf16String};

use crate::bytecode::executable::StaticPropertyLookupCacheSite;
use crate::bytecode::op;
use crate::bytecode::property_access::get_own_property_without_side_effects;
use crate::debugger::PauseReason;
use crate::interpreter::runtime_functions::{SlowPathControl, asm_try, handle_asm_exception};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::async_from_sync_iterator_prototype::create_async_from_sync_iterator;
use crate::runtime::async_generator::AsyncGenerator;
use crate::runtime::completion::{Completion, Must, completion_type_from_bytecode};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::generator_object::GeneratorObject;
use crate::runtime::iterator::{
    IteratorRecord, IteratorRecordImpl, get_iterator_from_method_impl, get_iterator_impl, get_iterator_values,
    iterator_close, iterator_hint_from_bytecode, iterator_next, iterator_step_value,
};
use crate::runtime::map::Map;
use crate::runtime::map_iterator::map_iteration_is_unobservable;
use crate::runtime::object::{IndexedStorageKind, IntegrityLevel};
use crate::runtime::property_attributes::{Attribute, DEFAULT_ATTRIBUTES, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
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
    let context = vm.running_execution_context().expect("a generator's frame is running");
    // SAFETY: The running context is live while the slow path runs in it.
    unsafe { context.as_ref() }
}
