/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime's implementation of the slow paths the interpreter calls, as in Libraries/LibJS/Interpreter/SlowPaths.cpp.
//! Each group lives in its own module, and the methods here hand each call to it.

pub mod bindings;
pub mod calls;
pub mod control;
pub mod operators;
pub mod property_access;

use core::cell::Cell;

use super::runtime_functions::{Runtime, RuntimeFunctions, SlowPathControl, handle_asm_exception};
use super::vm::Vm;
use crate::bytecode::executable::PropertyLookupCache;
use crate::bytecode::op;
use crate::layout::value::Value;

/// The VM a helper receives as an integer argument.
fn vm_from_helper_argument<'vm>(argument: u64) -> &'vm Vm {
    // SAFETY: The interpreter passes the VM it runs on, whose address escaped to it through the FFI call that started
    // it, and the VM outlives every call the interpreter makes into the runtime.
    unsafe { &*core::ptr::with_exposed_provenance::<Vm>(argument as usize) }
}

impl RuntimeFunctions for Runtime {
    // Arithmetic, comparisons, conversions and the jumps on comparisons: operators.rs.

    fn helper_to_boolean(encoded_value: u64) -> u64 {
        operators::helper_to_boolean(encoded_value)
    }

    fn helper_math_exp(encoded_value: u64) -> u64 {
        operators::helper_math_exp(encoded_value)
    }

    fn helper_empty_string(vm: u64) -> u64 {
        operators::helper_empty_string(vm_from_helper_argument(vm))
    }

    fn helper_single_ascii_character_string(vm: u64, encoded_value: u64) -> u64 {
        operators::helper_single_ascii_character_string(vm_from_helper_argument(vm), encoded_value)
    }

    fn helper_single_utf16_code_unit_string(vm: u64, encoded_value: u64) -> u64 {
        operators::helper_single_utf16_code_unit_string(vm_from_helper_argument(vm), encoded_value)
    }

    fn add_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::add_values(vm, pc, dst, lhs, rhs)
    }

    fn sub_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::sub_values(vm, pc, dst, lhs, rhs)
    }

    fn mul_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::mul_values(vm, pc, dst, lhs, rhs)
    }

    fn div_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::div_values(vm, pc, dst, lhs, rhs)
    }

    fn mod_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::mod_values(vm, pc, dst, lhs, rhs)
    }

    fn exp_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::exp_values(vm, pc, dst, lhs, rhs)
    }

    fn bitwise_and_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::bitwise_and_values(vm, pc, dst, lhs, rhs)
    }

    fn bitwise_or_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::bitwise_or_values(vm, pc, dst, lhs, rhs)
    }

    fn bitwise_xor_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::bitwise_xor_values(vm, pc, dst, lhs, rhs)
    }

    fn left_shift_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::left_shift_values(vm, pc, dst, lhs, rhs)
    }

    fn right_shift_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::right_shift_values(vm, pc, dst, lhs, rhs)
    }

    fn unsigned_right_shift_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::unsigned_right_shift_values(vm, pc, dst, lhs, rhs)
    }

    fn less_than_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::less_than_values(vm, pc, dst, lhs, rhs)
    }

    fn less_than_equals_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::less_than_equals_values(vm, pc, dst, lhs, rhs)
    }

    fn greater_than_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::greater_than_values(vm, pc, dst, lhs, rhs)
    }

    fn greater_than_equals_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::greater_than_equals_values(vm, pc, dst, lhs, rhs)
    }

    fn loosely_equals_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::loosely_equals_values(vm, pc, dst, lhs, rhs)
    }

    fn loosely_inequals_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::loosely_inequals_values(vm, pc, dst, lhs, rhs)
    }

    fn strictly_equals_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::strictly_equals_values(vm, pc, dst, lhs, rhs)
    }

    fn strictly_inequals_values(vm: &Vm, pc: u32, dst: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
        operators::strictly_inequals_values(vm, pc, dst, lhs, rhs)
    }

    fn jump_less_than_values(
        vm: &Vm,
        pc: u32,
        lhs: Value,
        rhs: Value,
        true_target: u32,
        false_target: u32,
    ) -> SlowPathControl {
        operators::jump_less_than_values(vm, pc, lhs, rhs, true_target, false_target)
    }

    fn jump_less_than_equals_values(
        vm: &Vm,
        pc: u32,
        lhs: Value,
        rhs: Value,
        true_target: u32,
        false_target: u32,
    ) -> SlowPathControl {
        operators::jump_less_than_equals_values(vm, pc, lhs, rhs, true_target, false_target)
    }

    fn jump_greater_than_values(
        vm: &Vm,
        pc: u32,
        lhs: Value,
        rhs: Value,
        true_target: u32,
        false_target: u32,
    ) -> SlowPathControl {
        operators::jump_greater_than_values(vm, pc, lhs, rhs, true_target, false_target)
    }

    fn jump_greater_than_equals_values(
        vm: &Vm,
        pc: u32,
        lhs: Value,
        rhs: Value,
        true_target: u32,
        false_target: u32,
    ) -> SlowPathControl {
        operators::jump_greater_than_equals_values(vm, pc, lhs, rhs, true_target, false_target)
    }

    fn jump_loosely_equals_values(
        vm: &Vm,
        pc: u32,
        lhs: Value,
        rhs: Value,
        true_target: u32,
        false_target: u32,
    ) -> SlowPathControl {
        operators::jump_loosely_equals_values(vm, pc, lhs, rhs, true_target, false_target)
    }

    fn jump_loosely_inequals_values(
        vm: &Vm,
        pc: u32,
        lhs: Value,
        rhs: Value,
        true_target: u32,
        false_target: u32,
    ) -> SlowPathControl {
        operators::jump_loosely_inequals_values(vm, pc, lhs, rhs, true_target, false_target)
    }

    fn jump_strictly_equals_values(
        _vm: &Vm,
        _pc: u32,
        lhs: Value,
        rhs: Value,
        true_target: u32,
        false_target: u32,
    ) -> SlowPathControl {
        operators::jump_strictly_equals_values(lhs, rhs, true_target, false_target)
    }

    fn jump_strictly_inequals_values(
        _vm: &Vm,
        _pc: u32,
        lhs: Value,
        rhs: Value,
        true_target: u32,
        false_target: u32,
    ) -> SlowPathControl {
        operators::jump_strictly_inequals_values(lhs, rhs, true_target, false_target)
    }

    fn unary_minus(
        vm: &Vm,
        pc: u32,
        _instruction: &op::UnaryMinus,
        values: &mut op::UnaryMinusValues,
    ) -> SlowPathControl {
        operators::unary_minus(vm, pc, values)
    }

    fn unary_plus(vm: &Vm, pc: u32, _instruction: &op::UnaryPlus, values: &mut op::UnaryPlusValues) -> SlowPathControl {
        operators::unary_plus(vm, pc, values)
    }

    fn bitwise_not(
        vm: &Vm,
        pc: u32,
        _instruction: &op::BitwiseNot,
        values: &mut op::BitwiseNotValues,
    ) -> SlowPathControl {
        operators::bitwise_not(vm, pc, values)
    }

    fn increment(vm: &Vm, pc: u32, _instruction: &op::Increment, values: &mut op::IncrementValues) -> SlowPathControl {
        operators::increment(vm, pc, values)
    }

    fn decrement(vm: &Vm, pc: u32, _instruction: &op::Decrement, values: &mut op::DecrementValues) -> SlowPathControl {
        operators::decrement(vm, pc, values)
    }

    fn postfix_increment(
        vm: &Vm,
        pc: u32,
        _instruction: &op::PostfixIncrement,
        values: &mut op::PostfixIncrementValues,
    ) -> SlowPathControl {
        operators::postfix_increment(vm, pc, values)
    }

    fn postfix_decrement(
        vm: &Vm,
        pc: u32,
        _instruction: &op::PostfixDecrement,
        values: &mut op::PostfixDecrementValues,
    ) -> SlowPathControl {
        operators::postfix_decrement(vm, pc, values)
    }

    fn to_int32(vm: &Vm, pc: u32, _instruction: &op::ToInt32, values: &mut op::ToInt32Values) -> SlowPathControl {
        operators::to_int32(vm, pc, values)
    }

    fn to_length(vm: &Vm, pc: u32, _instruction: &op::ToLength, values: &mut op::ToLengthValues) -> SlowPathControl {
        operators::to_length(vm, pc, values)
    }

    fn to_object(vm: &Vm, pc: u32, _instruction: &op::ToObject, values: &mut op::ToObjectValues) -> SlowPathControl {
        operators::to_object(vm, pc, values)
    }

    fn to_primitive_with_string_hint(
        vm: &Vm,
        pc: u32,
        _instruction: &op::ToPrimitiveWithStringHint,
        values: &mut op::ToPrimitiveWithStringHintValues,
    ) -> SlowPathControl {
        operators::to_primitive_with_string_hint(vm, pc, values)
    }

    fn to_string(vm: &Vm, pc: u32, _instruction: &op::ToString, values: &mut op::ToStringValues) -> SlowPathControl {
        operators::to_string(vm, pc, values)
    }

    fn r#typeof(vm: &Vm, pc: u32, _instruction: &op::Typeof, values: &mut op::TypeofValues) -> SlowPathControl {
        operators::r#typeof(vm, pc, values)
    }

    fn concat_string(
        vm: &Vm,
        pc: u32,
        _instruction: &op::ConcatString,
        values: &mut op::ConcatStringValues,
    ) -> SlowPathControl {
        operators::concat_string(vm, pc, values)
    }

    fn r#in(vm: &Vm, pc: u32, _instruction: &op::In, values: &mut op::InValues) -> SlowPathControl {
        operators::r#in(vm, pc, values)
    }

    fn instance_of(
        vm: &Vm,
        pc: u32,
        _instruction: &op::InstanceOf,
        values: &mut op::InstanceOfValues,
    ) -> SlowPathControl {
        operators::instance_of(vm, pc, values)
    }

    fn is_constructor(
        _vm: &Vm,
        pc: u32,
        _instruction: &op::IsConstructor,
        values: &mut op::IsConstructorValues,
    ) -> SlowPathControl {
        operators::is_constructor(pc, values)
    }

    // Property access and its inline caches: property_access.rs.

    fn get_by_id(vm: &Vm, pc: u32, instruction: &op::GetById, values: &mut op::GetByIdValues) -> SlowPathControl {
        property_access::get_by_id(vm, pc, instruction, values)
    }

    fn get_by_id_cached_accessor(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetById,
        values: &mut op::GetByIdValues,
    ) -> SlowPathControl {
        property_access::get_by_id_cached_accessor(vm, pc, instruction, values)
    }

    fn get_by_id_with_this(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetByIdWithThis,
        values: &mut op::GetByIdWithThisValues,
    ) -> SlowPathControl {
        property_access::get_by_id_with_this(vm, pc, instruction, values)
    }

    fn get_by_value(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetByValue,
        values: &mut op::GetByValueValues,
    ) -> SlowPathControl {
        property_access::get_by_value(vm, pc, instruction, values)
    }

    fn get_by_value_with_this(
        vm: &Vm,
        pc: u32,
        _instruction: &op::GetByValueWithThis,
        values: &mut op::GetByValueWithThisValues,
    ) -> SlowPathControl {
        property_access::get_by_value_with_this(vm, pc, values)
    }

    fn get_length(vm: &Vm, pc: u32, instruction: &op::GetLength, values: &mut op::GetLengthValues) -> SlowPathControl {
        property_access::get_length(vm, pc, instruction, values)
    }

    fn get_length_with_this(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetLengthWithThis,
        values: &mut op::GetLengthWithThisValues,
    ) -> SlowPathControl {
        property_access::get_length_with_this(vm, pc, instruction, values)
    }

    fn get_method(vm: &Vm, pc: u32, instruction: &op::GetMethod, values: &mut op::GetMethodValues) -> SlowPathControl {
        property_access::get_method(vm, pc, instruction, values)
    }

    fn put_by_id(vm: &Vm, pc: u32, instruction: &op::PutById, values: &mut op::PutByIdValues) -> SlowPathControl {
        property_access::put_by_id(vm, pc, instruction, values)
    }

    fn put_by_id_with_this(
        vm: &Vm,
        pc: u32,
        instruction: &op::PutByIdWithThis,
        values: &mut op::PutByIdWithThisValues,
    ) -> SlowPathControl {
        property_access::put_by_id_with_this(vm, pc, instruction, values)
    }

    fn put_by_value(
        vm: &Vm,
        pc: u32,
        instruction: &op::PutByValue,
        values: &mut op::PutByValueValues,
    ) -> SlowPathControl {
        property_access::put_by_value(vm, pc, instruction, values)
    }

    fn put_by_value_with_this(
        vm: &Vm,
        pc: u32,
        instruction: &op::PutByValueWithThis,
        values: &mut op::PutByValueWithThisValues,
    ) -> SlowPathControl {
        property_access::put_by_value_with_this(vm, pc, instruction, values)
    }

    fn put_by_spread(
        vm: &Vm,
        pc: u32,
        _instruction: &op::PutBySpread,
        values: &mut op::PutBySpreadValues,
    ) -> SlowPathControl {
        property_access::put_by_spread(vm, pc, values)
    }

    fn delete_by_id(
        vm: &Vm,
        pc: u32,
        instruction: &op::DeleteById,
        values: &mut op::DeleteByIdValues,
    ) -> SlowPathControl {
        property_access::delete_by_id(vm, pc, instruction, values)
    }

    fn delete_by_value(
        vm: &Vm,
        pc: u32,
        instruction: &op::DeleteByValue,
        values: &mut op::DeleteByValueValues,
    ) -> SlowPathControl {
        property_access::delete_by_value(vm, pc, instruction, values)
    }

    fn get_private_by_id(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetPrivateById,
        values: &mut op::GetPrivateByIdValues,
    ) -> SlowPathControl {
        property_access::get_private_by_id(vm, pc, instruction, values)
    }

    fn put_private_by_id(
        vm: &Vm,
        pc: u32,
        instruction: &op::PutPrivateById,
        values: &mut op::PutPrivateByIdValues,
    ) -> SlowPathControl {
        property_access::put_private_by_id(vm, pc, instruction, values)
    }

    fn has_private_id(
        vm: &Vm,
        pc: u32,
        instruction: &op::HasPrivateId,
        values: &mut op::HasPrivateIdValues,
    ) -> SlowPathControl {
        property_access::has_private_id(vm, pc, instruction, values)
    }

    fn add_private_name(
        vm: &Vm,
        pc: u32,
        instruction: &op::AddPrivateName,
        _values: &mut op::AddPrivateNameValues,
    ) -> SlowPathControl {
        property_access::add_private_name(vm, pc, instruction)
    }

    fn init_object_literal_property(
        vm: &Vm,
        pc: u32,
        instruction: &op::InitObjectLiteralProperty,
        values: &mut op::InitObjectLiteralPropertyValues,
    ) -> SlowPathControl {
        property_access::init_object_literal_property(vm, pc, instruction, values)
    }

    fn cache_object_shape(
        vm: &Vm,
        pc: u32,
        instruction: &op::CacheObjectShape,
        values: &mut op::CacheObjectShapeValues,
    ) -> SlowPathControl {
        property_access::cache_object_shape(vm, pc, instruction, values)
    }

    fn new_object(vm: &Vm, pc: u32, instruction: &op::NewObject, values: &mut op::NewObjectValues) -> SlowPathControl {
        property_access::new_object(vm, pc, instruction, values)
    }

    fn new_object_with_no_prototype(
        vm: &Vm,
        pc: u32,
        _instruction: &op::NewObjectWithNoPrototype,
        values: &mut op::NewObjectWithNoPrototypeValues,
    ) -> SlowPathControl {
        property_access::new_object_with_no_prototype(vm, pc, values)
    }

    fn copy_object_excluding_properties(
        vm: &Vm,
        pc: u32,
        instruction: &op::CopyObjectExcludingProperties,
        values: &mut op::CopyObjectExcludingPropertiesValues,
        excluded_names: &mut [Value],
    ) -> SlowPathControl {
        property_access::copy_object_excluding_properties(vm, pc, instruction, values, excluded_names)
    }

    fn create_data_property_or_throw(
        vm: &Vm,
        pc: u32,
        _instruction: &op::CreateDataPropertyOrThrow,
        values: &mut op::CreateDataPropertyOrThrowValues,
    ) -> SlowPathControl {
        property_access::create_data_property_or_throw(vm, pc, values)
    }

    fn get_object_property_iterator(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetObjectPropertyIterator,
        values: &mut op::GetObjectPropertyIteratorValues,
    ) -> SlowPathControl {
        property_access::get_object_property_iterator(vm, pc, instruction, values)
    }

    fn object_property_iterator_next(
        vm: &Vm,
        pc: u32,
        _instruction: &op::ObjectPropertyIteratorNext,
        values: &mut op::ObjectPropertyIteratorNextValues,
    ) -> SlowPathControl {
        property_access::object_property_iterator_next(vm, pc, values)
    }

    fn try_get_by_id_cache(encoded_base: u64, cache_address: u64) -> u64 {
        // SAFETY: The interpreter passes the address of one of the running executable's property lookup caches.
        let cache = unsafe { &*core::ptr::with_exposed_provenance::<PropertyLookupCache>(cache_address as usize) };
        property_access::try_get_by_id_cache(Value(encoded_base), cache).0
    }

    fn try_get_by_value_typed_array(
        _vm: &Vm,
        pc: u32,
        _instruction: &op::GetByValue,
        values: &mut op::GetByValueValues,
    ) -> bool {
        property_access::try_get_by_value_typed_array(pc, values)
    }

    fn try_inline_get_by_id_accessor(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetById,
        values: &mut op::GetByIdValues,
    ) -> bool {
        property_access::try_inline_get_by_id_accessor(vm, pc, instruction, values)
    }

    fn try_put_by_id_cache(vm: &Vm, _pc: u32, instruction: &op::PutById, values: &mut op::PutByIdValues) -> bool {
        property_access::try_put_by_id_cache(vm, instruction, values)
    }

    fn try_put_by_value_holey_array(
        _vm: &Vm,
        _pc: u32,
        _instruction: &op::PutByValue,
        values: &mut op::PutByValueValues,
    ) -> bool {
        property_access::try_put_by_value_holey_array(values)
    }

    fn try_put_by_value_typed_array(
        _vm: &Vm,
        pc: u32,
        _instruction: &op::PutByValue,
        values: &mut op::PutByValueValues,
    ) -> bool {
        property_access::try_put_by_value_typed_array(pc, values)
    }

    // Bindings and environments: bindings.rs.

    fn create_mutable_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreateMutableBinding,
        values: &mut op::CreateMutableBindingValues,
    ) -> SlowPathControl {
        bindings::create_mutable_binding(vm, pc, instruction, values)
    }

    fn create_immutable_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreateImmutableBinding,
        values: &mut op::CreateImmutableBindingValues,
    ) -> SlowPathControl {
        bindings::create_immutable_binding(vm, pc, instruction, values)
    }

    fn create_lexical_environment(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreateLexicalEnvironment,
        values: &mut op::CreateLexicalEnvironmentValues,
    ) -> SlowPathControl {
        bindings::create_lexical_environment_slow_path(vm, pc, instruction, values)
    }

    fn create_variable_environment(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreateVariableEnvironment,
        values: &mut op::CreateVariableEnvironmentValues,
    ) -> SlowPathControl {
        bindings::create_variable_environment_slow_path(vm, pc, instruction, values)
    }

    fn create_variable(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreateVariable,
        values: &mut op::CreateVariableValues,
    ) -> SlowPathControl {
        bindings::create_variable_slow_path(vm, pc, instruction, values)
    }

    fn create_private_environment(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreatePrivateEnvironment,
        values: &mut op::CreatePrivateEnvironmentValues,
    ) -> SlowPathControl {
        bindings::create_private_environment(vm, pc, instruction, values)
    }

    fn dynamic_get_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicGetBinding,
        values: &mut op::DynamicGetBindingValues,
    ) -> SlowPathControl {
        bindings::dynamic_get_binding(vm, pc, instruction, values)
    }

    fn dynamic_get_initialized_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicGetInitializedBinding,
        values: &mut op::DynamicGetInitializedBindingValues,
    ) -> SlowPathControl {
        bindings::dynamic_get_initialized_binding(vm, pc, instruction, values)
    }

    fn dynamic_get_callee_and_this(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicGetCalleeAndThisFromEnvironment,
        values: &mut op::DynamicGetCalleeAndThisFromEnvironmentValues,
    ) -> SlowPathControl {
        bindings::dynamic_get_callee_and_this(vm, pc, instruction, values)
    }

    fn dynamic_initialize_lexical_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicInitializeLexicalBinding,
        values: &mut op::DynamicInitializeLexicalBindingValues,
    ) -> SlowPathControl {
        bindings::dynamic_initialize_lexical_binding(vm, pc, instruction, values)
    }

    fn dynamic_initialize_variable_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicInitializeVariableBinding,
        values: &mut op::DynamicInitializeVariableBindingValues,
    ) -> SlowPathControl {
        bindings::dynamic_initialize_variable_binding(vm, pc, instruction, values)
    }

    fn dynamic_set_lexical_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicSetLexicalBinding,
        values: &mut op::DynamicSetLexicalBindingValues,
    ) -> SlowPathControl {
        bindings::dynamic_set_lexical_binding(vm, pc, instruction, values)
    }

    fn dynamic_set_variable_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicSetVariableBinding,
        values: &mut op::DynamicSetVariableBindingValues,
    ) -> SlowPathControl {
        bindings::dynamic_set_variable_binding(vm, pc, instruction, values)
    }

    fn dynamic_typeof_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicTypeofBinding,
        values: &mut op::DynamicTypeofBindingValues,
    ) -> SlowPathControl {
        bindings::dynamic_typeof_binding(vm, pc, instruction, values)
    }

    fn enter_object_environment(
        vm: &Vm,
        pc: u32,
        instruction: &op::EnterObjectEnvironment,
        values: &mut op::EnterObjectEnvironmentValues,
    ) -> SlowPathControl {
        bindings::enter_object_environment(vm, pc, instruction, values)
    }

    fn get_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetBinding,
        values: &mut op::GetBindingValues,
    ) -> SlowPathControl {
        bindings::get_binding(vm, pc, instruction, values)
    }

    fn get_callee_and_this(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetCalleeAndThisFromEnvironment,
        values: &mut op::GetCalleeAndThisFromEnvironmentValues,
    ) -> SlowPathControl {
        bindings::get_callee_and_this(vm, pc, instruction, values)
    }

    fn get_global(vm: &Vm, pc: u32, instruction: &op::GetGlobal, values: &mut op::GetGlobalValues) -> SlowPathControl {
        bindings::get_global(vm, pc, instruction, values)
    }

    fn set_global(vm: &Vm, pc: u32, instruction: &op::SetGlobal, values: &mut op::SetGlobalValues) -> SlowPathControl {
        bindings::set_global(vm, pc, instruction, values)
    }

    fn get_import(vm: &Vm, pc: u32, instruction: &op::GetImport, values: &mut op::GetImportValues) -> SlowPathControl {
        bindings::get_import(vm, pc, instruction, values)
    }

    fn get_import_meta(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetImportMeta,
        values: &mut op::GetImportMetaValues,
    ) -> SlowPathControl {
        bindings::get_import_meta(vm, pc, instruction, values)
    }

    fn import_call(
        vm: &Vm,
        pc: u32,
        instruction: &op::ImportCall,
        values: &mut op::ImportCallValues,
    ) -> SlowPathControl {
        bindings::import_call(vm, pc, instruction, values)
    }

    fn get_new_target(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetNewTarget,
        values: &mut op::GetNewTargetValues,
    ) -> SlowPathControl {
        bindings::get_new_target(vm, pc, instruction, values)
    }

    fn resolve_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::ResolveBinding,
        values: &mut op::ResolveBindingValues,
    ) -> SlowPathControl {
        bindings::resolve_binding(vm, pc, instruction, values)
    }

    fn resolve_super_base(
        vm: &Vm,
        pc: u32,
        instruction: &op::ResolveSuperBase,
        values: &mut op::ResolveSuperBaseValues,
    ) -> SlowPathControl {
        bindings::resolve_super_base(vm, pc, instruction, values)
    }

    fn resolve_this_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::ResolveThisBinding,
        values: &mut op::ResolveThisBindingValues,
    ) -> SlowPathControl {
        bindings::resolve_this_binding(vm, pc, instruction, values)
    }

    fn set_lexical_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::SetLexicalBinding,
        values: &mut op::SetLexicalBindingValues,
    ) -> SlowPathControl {
        bindings::set_lexical_binding(vm, pc, instruction, values)
    }

    fn set_variable_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::SetVariableBinding,
        values: &mut op::SetVariableBindingValues,
    ) -> SlowPathControl {
        bindings::set_variable_binding(vm, pc, instruction, values)
    }

    fn set_resolved_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::SetResolvedBinding,
        values: &mut op::SetResolvedBindingValues,
    ) -> SlowPathControl {
        bindings::set_resolved_binding(vm, pc, instruction, values)
    }

    fn delete_variable(
        vm: &Vm,
        pc: u32,
        instruction: &op::DeleteVariable,
        values: &mut op::DeleteVariableValues,
    ) -> SlowPathControl {
        bindings::delete_variable(vm, pc, instruction, values)
    }

    fn typeof_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::TypeofBinding,
        values: &mut op::TypeofBindingValues,
    ) -> SlowPathControl {
        bindings::typeof_binding(vm, pc, instruction, values)
    }

    fn verify_environment_coordinate(
        vm: &Vm,
        pc: u32,
        instruction: &op::VerifyEnvironmentCoordinate,
        values: &mut op::VerifyEnvironmentCoordinateValues,
    ) -> SlowPathControl {
        bindings::verify_environment_coordinate(vm, pc, instruction, values)
    }

    // Calls, functions and classes: calls.rs.

    fn try_inline_call(
        vm: &Vm,
        pc: u32,
        instruction: &op::Call,
        values: &mut op::CallValues,
        arguments: &mut [Value],
    ) -> bool {
        calls::try_inline_call(vm, pc, instruction, values, arguments)
    }

    fn create_rest_params(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreateRestParams,
        values: &mut op::CreateRestParamsValues,
    ) -> SlowPathControl {
        calls::create_rest_params(vm, pc, instruction, values)
    }

    fn create_arguments(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreateArguments,
        values: &mut op::CreateArgumentsValues,
    ) -> SlowPathControl {
        calls::create_arguments(vm, pc, instruction, values)
    }

    fn stack_overflow(vm: &Vm, pc: u32) -> SlowPathControl {
        calls::stack_overflow(vm, pc)
    }

    fn get_super_constructor(
        vm: &Vm,
        pc: u32,
        _instruction: &op::GetSuperConstructor,
        values: &mut op::GetSuperConstructorValues,
    ) -> SlowPathControl {
        calls::get_super_constructor(vm, pc, values)
    }

    fn new_class(
        vm: &Vm,
        pc: u32,
        instruction: &op::NewClass,
        values: &mut op::NewClassValues,
        element_keys: &mut [Value],
    ) -> SlowPathControl {
        calls::new_class(vm, pc, instruction, values, element_keys)
    }

    fn call(
        vm: &Vm,
        pc: u32,
        instruction: &op::Call,
        values: &mut op::CallValues,
        arguments: &mut [Value],
    ) -> SlowPathControl {
        calls::call(vm, pc, instruction, values, arguments)
    }

    fn call_direct_eval(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallDirectEval,
        values: &mut op::CallDirectEvalValues,
        arguments: &mut [Value],
    ) -> SlowPathControl {
        calls::call_direct_eval(vm, pc, instruction, values, arguments)
    }

    fn call_with_argument_array(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallWithArgumentArray,
        values: &mut op::CallWithArgumentArrayValues,
    ) -> SlowPathControl {
        calls::call_with_argument_array(vm, pc, instruction, values)
    }

    fn call_direct_eval_with_argument_array(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallDirectEvalWithArgumentArray,
        values: &mut op::CallDirectEvalWithArgumentArrayValues,
    ) -> SlowPathControl {
        calls::call_direct_eval_with_argument_array(vm, pc, instruction, values)
    }

    fn call_builtin_math_abs(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinMathAbs,
        values: &mut op::CallBuiltinMathAbsValues,
    ) -> SlowPathControl {
        calls::call_builtin_math_abs(vm, pc, instruction, values)
    }

    fn call_builtin_math_log(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinMathLog,
        values: &mut op::CallBuiltinMathLogValues,
    ) -> SlowPathControl {
        calls::call_builtin_math_log(vm, pc, instruction, values)
    }

    fn call_builtin_math_pow(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinMathPow,
        values: &mut op::CallBuiltinMathPowValues,
    ) -> SlowPathControl {
        calls::call_builtin_math_pow(vm, pc, instruction, values)
    }

    fn call_builtin_math_exp(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinMathExp,
        values: &mut op::CallBuiltinMathExpValues,
    ) -> SlowPathControl {
        calls::call_builtin_math_exp(vm, pc, instruction, values)
    }

    fn call_builtin_math_ceil(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinMathCeil,
        values: &mut op::CallBuiltinMathCeilValues,
    ) -> SlowPathControl {
        calls::call_builtin_math_ceil(vm, pc, instruction, values)
    }

    fn call_builtin_math_floor(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinMathFloor,
        values: &mut op::CallBuiltinMathFloorValues,
    ) -> SlowPathControl {
        calls::call_builtin_math_floor(vm, pc, instruction, values)
    }

    fn call_builtin_math_imul(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinMathImul,
        values: &mut op::CallBuiltinMathImulValues,
    ) -> SlowPathControl {
        calls::call_builtin_math_imul(vm, pc, instruction, values)
    }

    fn call_builtin_math_random(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinMathRandom,
        values: &mut op::CallBuiltinMathRandomValues,
    ) -> SlowPathControl {
        calls::call_builtin_math_random(vm, pc, instruction, values)
    }

    fn call_builtin_math_round(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinMathRound,
        values: &mut op::CallBuiltinMathRoundValues,
    ) -> SlowPathControl {
        calls::call_builtin_math_round(vm, pc, instruction, values)
    }

    fn call_builtin_math_sqrt(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinMathSqrt,
        values: &mut op::CallBuiltinMathSqrtValues,
    ) -> SlowPathControl {
        calls::call_builtin_math_sqrt(vm, pc, instruction, values)
    }

    fn call_builtin_math_sin(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinMathSin,
        values: &mut op::CallBuiltinMathSinValues,
    ) -> SlowPathControl {
        calls::call_builtin_math_sin(vm, pc, instruction, values)
    }

    fn call_builtin_math_cos(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinMathCos,
        values: &mut op::CallBuiltinMathCosValues,
    ) -> SlowPathControl {
        calls::call_builtin_math_cos(vm, pc, instruction, values)
    }

    fn call_builtin_math_tan(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinMathTan,
        values: &mut op::CallBuiltinMathTanValues,
    ) -> SlowPathControl {
        calls::call_builtin_math_tan(vm, pc, instruction, values)
    }

    fn call_builtin_regexp_prototype_exec(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinRegExpPrototypeExec,
        values: &mut op::CallBuiltinRegExpPrototypeExecValues,
    ) -> SlowPathControl {
        calls::call_builtin_regexp_prototype_exec(vm, pc, instruction, values)
    }

    fn call_builtin_regexp_prototype_replace(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinRegExpPrototypeReplace,
        values: &mut op::CallBuiltinRegExpPrototypeReplaceValues,
    ) -> SlowPathControl {
        calls::call_builtin_regexp_prototype_replace(vm, pc, instruction, values)
    }

    fn call_builtin_regexp_prototype_split(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinRegExpPrototypeSplit,
        values: &mut op::CallBuiltinRegExpPrototypeSplitValues,
    ) -> SlowPathControl {
        calls::call_builtin_regexp_prototype_split(vm, pc, instruction, values)
    }

    fn call_builtin_ordinary_has_instance(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinOrdinaryHasInstance,
        values: &mut op::CallBuiltinOrdinaryHasInstanceValues,
    ) -> SlowPathControl {
        calls::call_builtin_ordinary_has_instance(vm, pc, instruction, values)
    }

    fn call_builtin_array_iterator_prototype_next(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinArrayIteratorPrototypeNext,
        values: &mut op::CallBuiltinArrayIteratorPrototypeNextValues,
    ) -> SlowPathControl {
        calls::call_builtin_array_iterator_prototype_next(vm, pc, instruction, values)
    }

    fn call_builtin_map_iterator_prototype_next(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinMapIteratorPrototypeNext,
        values: &mut op::CallBuiltinMapIteratorPrototypeNextValues,
    ) -> SlowPathControl {
        calls::call_builtin_map_iterator_prototype_next(vm, pc, instruction, values)
    }

    fn call_builtin_set_iterator_prototype_next(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinSetIteratorPrototypeNext,
        values: &mut op::CallBuiltinSetIteratorPrototypeNextValues,
    ) -> SlowPathControl {
        calls::call_builtin_set_iterator_prototype_next(vm, pc, instruction, values)
    }

    fn call_builtin_string_iterator_prototype_next(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinStringIteratorPrototypeNext,
        values: &mut op::CallBuiltinStringIteratorPrototypeNextValues,
    ) -> SlowPathControl {
        calls::call_builtin_string_iterator_prototype_next(vm, pc, instruction, values)
    }

    fn call_builtin_string_from_char_code(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinStringFromCharCode,
        values: &mut op::CallBuiltinStringFromCharCodeValues,
    ) -> SlowPathControl {
        calls::call_builtin_string_from_char_code(vm, pc, instruction, values)
    }

    fn call_builtin_string_prototype_char_code_at(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinStringPrototypeCharCodeAt,
        values: &mut op::CallBuiltinStringPrototypeCharCodeAtValues,
    ) -> SlowPathControl {
        calls::call_builtin_string_prototype_char_code_at(vm, pc, instruction, values)
    }

    fn call_builtin_string_prototype_char_at(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallBuiltinStringPrototypeCharAt,
        values: &mut op::CallBuiltinStringPrototypeCharAtValues,
    ) -> SlowPathControl {
        calls::call_builtin_string_prototype_char_at(vm, pc, instruction, values)
    }

    fn call_construct(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallConstruct,
        values: &mut op::CallConstructValues,
        arguments: &mut [Value],
    ) -> SlowPathControl {
        calls::call_construct(vm, pc, instruction, values, arguments)
    }

    fn call_construct_with_argument_array(
        vm: &Vm,
        pc: u32,
        instruction: &op::CallConstructWithArgumentArray,
        values: &mut op::CallConstructWithArgumentArrayValues,
    ) -> SlowPathControl {
        calls::call_construct_with_argument_array(vm, pc, instruction, values)
    }

    fn super_call_with_argument_array(
        vm: &Vm,
        pc: u32,
        instruction: &op::SuperCallWithArgumentArray,
        values: &mut op::SuperCallWithArgumentArrayValues,
    ) -> SlowPathControl {
        calls::super_call_with_argument_array(vm, pc, instruction, values)
    }

    fn set_function_name(
        vm: &Vm,
        pc: u32,
        instruction: &op::SetFunctionName,
        values: &mut op::SetFunctionNameValues,
    ) -> SlowPathControl {
        calls::set_function_name(vm, pc, instruction, values)
    }

    fn new_function(
        vm: &Vm,
        pc: u32,
        instruction: &op::NewFunction,
        values: &mut op::NewFunctionValues,
    ) -> SlowPathControl {
        calls::new_function(vm, pc, instruction, values)
    }

    fn helper_handle_raw_native_exception(vm: u64, encoded_exception: u64) -> u64 {
        // SAFETY: The interpreter passes its VM.
        let vm = unsafe { &*core::ptr::with_exposed_provenance::<Vm>(vm as usize) };
        calls::handle_raw_native_exception(vm, Value(encoded_exception)).0 as u64
    }

    // Literals, iterators, generators and control flow: control.rs.

    fn debugger_check_breakpoint(vm: &Vm, pc: u32) {
        control::debugger_check_breakpoint(vm, pc);
    }

    fn fallback_handler(_vm: &Vm, pc: u32, _instruction: *const u8) -> SlowPathControl {
        control::fallback_handler(pc)
    }

    fn new_array(
        vm: &Vm,
        pc: u32,
        instruction: &op::NewArray,
        values: &mut op::NewArrayValues,
        elements: &mut [Value],
    ) -> SlowPathControl {
        control::new_array(vm, pc, instruction, values, elements)
    }

    fn new_primitive_array(
        vm: &Vm,
        pc: u32,
        instruction: &op::NewPrimitiveArray,
        values: &mut op::NewPrimitiveArrayValues,
    ) -> SlowPathControl {
        control::new_primitive_array(vm, pc, instruction, values)
    }

    fn new_array_with_length(
        vm: &Vm,
        pc: u32,
        _instruction: &op::NewArrayWithLength,
        values: &mut op::NewArrayWithLengthValues,
    ) -> SlowPathControl {
        control::new_array_with_length(vm, pc, values)
    }

    fn array_append(
        vm: &Vm,
        pc: u32,
        instruction: &op::ArrayAppend,
        values: &mut op::ArrayAppendValues,
    ) -> SlowPathControl {
        control::array_append(vm, pc, instruction, values)
    }

    fn get_template_object(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetTemplateObject,
        values: &mut op::GetTemplateObjectValues,
        strings: &mut [Value],
    ) -> SlowPathControl {
        control::get_template_object(vm, pc, instruction, values, strings)
    }

    fn new_regexp(vm: &Vm, pc: u32, instruction: &op::NewRegExp, _values: &mut op::NewRegExpValues) -> SlowPathControl {
        control::new_regexp(vm, pc, instruction)
    }

    fn new_reference_error(
        vm: &Vm,
        pc: u32,
        instruction: &op::NewReferenceError,
        values: &mut op::NewReferenceErrorValues,
    ) -> SlowPathControl {
        control::new_reference_error(vm, pc, instruction, values)
    }

    fn new_type_error(
        vm: &Vm,
        pc: u32,
        instruction: &op::NewTypeError,
        values: &mut op::NewTypeErrorValues,
    ) -> SlowPathControl {
        control::new_type_error(vm, pc, instruction, values)
    }

    fn get_iterator(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetIterator,
        values: &mut op::GetIteratorValues,
    ) -> SlowPathControl {
        control::get_iterator(vm, pc, instruction, values)
    }

    fn iterator_close(
        vm: &Vm,
        pc: u32,
        instruction: &op::IteratorClose,
        values: &mut op::IteratorCloseValues,
    ) -> SlowPathControl {
        control::iterator_close_slow_path(vm, pc, instruction, values)
    }

    fn iterator_next(
        vm: &Vm,
        pc: u32,
        _instruction: &op::IteratorNext,
        values: &mut op::IteratorNextValues,
    ) -> SlowPathControl {
        control::iterator_next_slow_path(vm, pc, values)
    }

    fn iterator_next_unpack(
        vm: &Vm,
        pc: u32,
        _instruction: &op::IteratorNextUnpack,
        values: &mut op::IteratorNextUnpackValues,
    ) -> SlowPathControl {
        control::iterator_next_unpack(vm, pc, values)
    }

    fn iterator_to_array(
        vm: &Vm,
        pc: u32,
        _instruction: &op::IteratorToArray,
        values: &mut op::IteratorToArrayValues,
    ) -> SlowPathControl {
        control::iterator_to_array(vm, pc, values)
    }

    fn create_async_from_sync_iterator(
        vm: &Vm,
        pc: u32,
        _instruction: &op::CreateAsyncFromSyncIterator,
        values: &mut op::CreateAsyncFromSyncIteratorValues,
    ) -> SlowPathControl {
        control::create_async_from_sync_iterator_slow_path(vm, pc, values)
    }

    fn get_completion_fields(
        vm: &Vm,
        pc: u32,
        _instruction: &op::GetCompletionFields,
        values: &mut op::GetCompletionFieldsValues,
    ) -> SlowPathControl {
        control::get_completion_fields(vm, pc, values)
    }

    fn set_completion_type(
        vm: &Vm,
        pc: u32,
        instruction: &op::SetCompletionType,
        values: &mut op::SetCompletionTypeValues,
    ) -> SlowPathControl {
        control::set_completion_type(vm, pc, instruction, values)
    }

    fn debugger(_vm: &Vm, pc: u32, _instruction: &op::Debugger, _values: &mut op::DebuggerValues) -> SlowPathControl {
        control::debugger(pc)
    }

    fn throw(vm: &Vm, pc: u32, _instruction: &op::Throw, values: &mut op::ThrowValues) -> SlowPathControl {
        handle_asm_exception(vm, pc, values.src)
    }

    fn throw_if_tdz(
        vm: &Vm,
        pc: u32,
        _instruction: &op::ThrowIfTDZ,
        values: &mut op::ThrowIfTDZValues,
    ) -> SlowPathControl {
        control::throw_if_tdz(vm, pc, values)
    }

    fn throw_if_not_object(
        vm: &Vm,
        pc: u32,
        _instruction: &op::ThrowIfNotObject,
        values: &mut op::ThrowIfNotObjectValues,
    ) -> SlowPathControl {
        control::throw_if_not_object(vm, pc, values)
    }

    fn throw_if_nullish(
        vm: &Vm,
        pc: u32,
        _instruction: &op::ThrowIfNullish,
        values: &mut op::ThrowIfNullishValues,
    ) -> SlowPathControl {
        control::throw_if_nullish(vm, pc, values)
    }

    fn throw_const_assignment(
        vm: &Vm,
        pc: u32,
        _instruction: &op::ThrowConstAssignment,
        _values: &mut op::ThrowConstAssignmentValues,
    ) -> SlowPathControl {
        control::throw_const_assignment(vm, pc)
    }

    fn r#await(vm: &Vm, _pc: u32, instruction: &op::Await, values: &mut op::AwaitValues) -> SlowPathControl {
        control::r#await(vm, instruction, values)
    }

    fn r#yield(vm: &Vm, _pc: u32, instruction: &op::Yield, values: &mut op::YieldValues) -> SlowPathControl {
        control::r#yield(vm, instruction, values)
    }

    fn yield_iterator_result(
        vm: &Vm,
        _pc: u32,
        instruction: &op::YieldIteratorResult,
        values: &mut op::YieldIteratorResultValues,
    ) -> SlowPathControl {
        control::yield_iterator_result(vm, instruction, values)
    }
}
