/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The slow paths for arithmetic, comparisons and conversions, and their helpers.

use core::cell::Cell;

use crate::bytecode::op;
use crate::interpreter::runtime_functions::{SlowPathControl, asm_try};
use crate::interpreter::vm::Vm;
use crate::layout::value::Value;
use crate::runtime::big_int::BigInt;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::value::{self, PreferredType};
use crate::utf16::Utf16View;

/// Stores the result of the binary operator instruction at `pc`, whose length is `length`, and continues after it.
/// NB: The slow paths that receive their operands in registers each serve the instruction of one opcode, so they know
///     its length.
fn finish_binary_slow_path_value(pc: u32, length: u32, destination: &Cell<Value>, result: Value) -> SlowPathControl {
    destination.set(result);
    SlowPathControl::continue_at(pc + length)
}

fn finish_binary_slow_path(
    vm: &Vm,
    pc: u32,
    length: u32,
    destination: &Cell<Value>,
    result: ThrowCompletionOr<Value>,
) -> SlowPathControl {
    finish_binary_slow_path_value(pc, length, destination, asm_try!(vm, pc, result))
}

fn finish_binary_slow_path_with_boolean(
    vm: &Vm,
    pc: u32,
    length: u32,
    destination: &Cell<Value>,
    result: ThrowCompletionOr<bool>,
) -> SlowPathControl {
    finish_binary_slow_path_value(pc, length, destination, Value::from_bool(asm_try!(vm, pc, result)))
}

pub fn add_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, op::Add::LENGTH, destination, value::add(vm, lhs, rhs))
}

pub fn sub_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, op::Sub::LENGTH, destination, value::sub(vm, lhs, rhs))
}

pub fn mul_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, op::Mul::LENGTH, destination, value::mul(vm, lhs, rhs))
}

pub fn div_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, op::Div::LENGTH, destination, value::div(vm, lhs, rhs))
}

pub fn less_than_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path_with_boolean(
        vm,
        pc,
        op::LessThan::LENGTH,
        destination,
        value::less_than(vm, lhs, rhs),
    )
}

pub fn less_than_equals_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path_with_boolean(
        vm,
        pc,
        op::LessThanEquals::LENGTH,
        destination,
        value::less_than_equals(vm, lhs, rhs),
    )
}

pub fn greater_than_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path_with_boolean(
        vm,
        pc,
        op::GreaterThan::LENGTH,
        destination,
        value::greater_than(vm, lhs, rhs),
    )
}

pub fn greater_than_equals_values(
    vm: &Vm,
    pc: u32,
    destination: &Cell<Value>,
    lhs: Value,
    rhs: Value,
) -> SlowPathControl {
    finish_binary_slow_path_with_boolean(
        vm,
        pc,
        op::GreaterThanEquals::LENGTH,
        destination,
        value::greater_than_equals(vm, lhs, rhs),
    )
}

pub fn increment(vm: &Vm, pc: u32, values: &mut op::IncrementValues) -> SlowPathControl {
    let old_value = asm_try!(vm, pc, values.dst.to_numeric(vm));
    if old_value.is_number() {
        values.dst = Value::from_f64(old_value.as_f64() + 1.0);
    } else {
        let result = old_value.as_bigint().big_integer() + 1;
        values.dst = Value::from_bigint(BigInt::create(vm, result));
    }
    SlowPathControl::continue_at(pc + op::Increment::LENGTH)
}

pub fn decrement(vm: &Vm, pc: u32, values: &mut op::DecrementValues) -> SlowPathControl {
    let old_value = asm_try!(vm, pc, values.dst.to_numeric(vm));
    if old_value.is_number() {
        values.dst = Value::from_f64(old_value.as_f64() - 1.0);
    } else {
        let result = old_value.as_bigint().big_integer() - 1;
        values.dst = Value::from_bigint(BigInt::create(vm, result));
    }
    SlowPathControl::continue_at(pc + op::Decrement::LENGTH)
}

/// Comparison jump slow paths return one of two target PCs.
fn jump_on_comparison(
    vm: &Vm,
    pc: u32,
    comparison: ThrowCompletionOr<bool>,
    true_target: u32,
    false_target: u32,
) -> SlowPathControl {
    if asm_try!(vm, pc, comparison) {
        return SlowPathControl::dispatch_at(true_target);
    }
    SlowPathControl::dispatch_at(false_target)
}

pub fn jump_less_than_values(
    vm: &Vm,
    pc: u32,
    lhs: Value,
    rhs: Value,
    true_target: u32,
    false_target: u32,
) -> SlowPathControl {
    jump_on_comparison(vm, pc, value::less_than(vm, lhs, rhs), true_target, false_target)
}

pub fn jump_greater_than_values(
    vm: &Vm,
    pc: u32,
    lhs: Value,
    rhs: Value,
    true_target: u32,
    false_target: u32,
) -> SlowPathControl {
    jump_on_comparison(vm, pc, value::greater_than(vm, lhs, rhs), true_target, false_target)
}

pub fn jump_less_than_equals_values(
    vm: &Vm,
    pc: u32,
    lhs: Value,
    rhs: Value,
    true_target: u32,
    false_target: u32,
) -> SlowPathControl {
    jump_on_comparison(vm, pc, value::less_than_equals(vm, lhs, rhs), true_target, false_target)
}

pub fn jump_greater_than_equals_values(
    vm: &Vm,
    pc: u32,
    lhs: Value,
    rhs: Value,
    true_target: u32,
    false_target: u32,
) -> SlowPathControl {
    jump_on_comparison(
        vm,
        pc,
        value::greater_than_equals(vm, lhs, rhs),
        true_target,
        false_target,
    )
}

pub fn jump_loosely_equals_values(
    vm: &Vm,
    pc: u32,
    lhs: Value,
    rhs: Value,
    true_target: u32,
    false_target: u32,
) -> SlowPathControl {
    if asm_try!(vm, pc, value::is_loosely_equal(vm, lhs, rhs)) {
        return SlowPathControl::dispatch_at(true_target);
    }
    SlowPathControl::dispatch_at(false_target)
}

pub fn jump_loosely_inequals_values(
    vm: &Vm,
    pc: u32,
    lhs: Value,
    rhs: Value,
    true_target: u32,
    false_target: u32,
) -> SlowPathControl {
    if !asm_try!(vm, pc, value::is_loosely_equal(vm, lhs, rhs)) {
        return SlowPathControl::dispatch_at(true_target);
    }
    SlowPathControl::dispatch_at(false_target)
}

pub fn jump_strictly_equals_values(lhs: Value, rhs: Value, true_target: u32, false_target: u32) -> SlowPathControl {
    if value::is_strictly_equal(lhs, rhs) {
        return SlowPathControl::dispatch_at(true_target);
    }
    SlowPathControl::dispatch_at(false_target)
}

pub fn jump_strictly_inequals_values(lhs: Value, rhs: Value, true_target: u32, false_target: u32) -> SlowPathControl {
    if !value::is_strictly_equal(lhs, rhs) {
        return SlowPathControl::dispatch_at(true_target);
    }
    SlowPathControl::dispatch_at(false_target)
}

pub fn postfix_increment(vm: &Vm, pc: u32, values: &mut op::PostfixIncrementValues) -> SlowPathControl {
    let old_value = asm_try!(vm, pc, values.src.to_numeric(vm));
    values.dst = old_value;
    if old_value.is_number() {
        values.src = Value::from_f64(old_value.as_f64() + 1.0);
    } else {
        let result = old_value.as_bigint().big_integer() + 1;
        values.src = Value::from_bigint(BigInt::create(vm, result));
    }
    SlowPathControl::continue_at(pc + op::PostfixIncrement::LENGTH)
}

pub fn concat_string(vm: &Vm, pc: u32, values: &mut op::ConcatStringValues) -> SlowPathControl {
    let string = asm_try!(vm, pc, values.src.to_primitive_string(vm));
    values.dst = Value::from_string(asm_try!(
        vm,
        pc,
        PrimitiveString::create_from_concatenation(vm, values.dst.as_string(), string)
    ));
    SlowPathControl::continue_at(pc + op::ConcatString::LENGTH)
}

pub fn exp_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, op::Exp::LENGTH, destination, value::exp(vm, lhs, rhs))
}

pub fn bitwise_xor_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(
        vm,
        pc,
        op::BitwiseXor::LENGTH,
        destination,
        value::bitwise_xor(vm, lhs, rhs),
    )
}

pub fn bitwise_and_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(
        vm,
        pc,
        op::BitwiseAnd::LENGTH,
        destination,
        value::bitwise_and(vm, lhs, rhs),
    )
}

pub fn bitwise_or_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(
        vm,
        pc,
        op::BitwiseOr::LENGTH,
        destination,
        value::bitwise_or(vm, lhs, rhs),
    )
}

pub fn left_shift_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(
        vm,
        pc,
        op::LeftShift::LENGTH,
        destination,
        value::left_shift(vm, lhs, rhs),
    )
}

pub fn right_shift_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(
        vm,
        pc,
        op::RightShift::LENGTH,
        destination,
        value::right_shift(vm, lhs, rhs),
    )
}

pub fn unsigned_right_shift_values(
    vm: &Vm,
    pc: u32,
    destination: &Cell<Value>,
    lhs: Value,
    rhs: Value,
) -> SlowPathControl {
    finish_binary_slow_path(
        vm,
        pc,
        op::UnsignedRightShift::LENGTH,
        destination,
        value::unsigned_right_shift(vm, lhs, rhs),
    )
}

pub fn mod_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, op::Mod::LENGTH, destination, value::r#mod(vm, lhs, rhs))
}

fn loosely_equals(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<bool> {
    if lhs.tag() == rhs.tag() && (lhs.is_int32() || lhs.is_object() || lhs.is_boolean() || lhs.is_nullish()) {
        return Ok(lhs.0 == rhs.0);
    }
    value::is_loosely_equal(vm, lhs, rhs)
}

fn loosely_inequals(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<bool> {
    Ok(!loosely_equals(vm, lhs, rhs)?)
}

fn strictly_equals(lhs: Value, rhs: Value) -> bool {
    if lhs.tag() == rhs.tag() && (lhs.is_int32() || lhs.is_object() || lhs.is_boolean() || lhs.is_nullish()) {
        return lhs.0 == rhs.0;
    }
    value::is_strictly_equal(lhs, rhs)
}

pub fn strictly_equals_values(pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path_value(
        pc,
        op::StrictlyEquals::LENGTH,
        destination,
        Value::from_bool(strictly_equals(lhs, rhs)),
    )
}

pub fn strictly_inequals_values(pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path_value(
        pc,
        op::StrictlyInequals::LENGTH,
        destination,
        Value::from_bool(!strictly_equals(lhs, rhs)),
    )
}

pub fn loosely_equals_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path_with_boolean(
        vm,
        pc,
        op::LooselyEquals::LENGTH,
        destination,
        loosely_equals(vm, lhs, rhs),
    )
}

pub fn loosely_inequals_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path_with_boolean(
        vm,
        pc,
        op::LooselyInequals::LENGTH,
        destination,
        loosely_inequals(vm, lhs, rhs),
    )
}

pub fn unary_minus(vm: &Vm, pc: u32, values: &mut op::UnaryMinusValues) -> SlowPathControl {
    values.dst = asm_try!(vm, pc, value::unary_minus(vm, values.src));
    SlowPathControl::continue_at(pc + op::UnaryMinus::LENGTH)
}

pub fn to_string(vm: &Vm, pc: u32, values: &mut op::ToStringValues) -> SlowPathControl {
    let result = asm_try!(vm, pc, values.value.to_primitive_string(vm));
    values.dst = Value::from_string(result);
    SlowPathControl::continue_at(pc + op::ToString::LENGTH)
}

pub fn to_primitive_with_string_hint(
    vm: &Vm,
    pc: u32,
    values: &mut op::ToPrimitiveWithStringHintValues,
) -> SlowPathControl {
    let result = asm_try!(vm, pc, values.value.to_primitive(vm, PreferredType::String));
    values.dst = result;
    SlowPathControl::continue_at(pc + op::ToPrimitiveWithStringHint::LENGTH)
}

pub fn to_object(vm: &Vm, pc: u32, values: &mut op::ToObjectValues) -> SlowPathControl {
    let result = asm_try!(vm, pc, values.value.to_object(vm));
    values.dst = Value::from_object(result);
    SlowPathControl::continue_at(pc + op::ToObject::LENGTH)
}

pub fn to_length(vm: &Vm, pc: u32, values: &mut op::ToLengthValues) -> SlowPathControl {
    let result = asm_try!(vm, pc, values.value.to_length(vm));
    values.dst = Value::from_f64(result as f64);
    SlowPathControl::continue_at(pc + op::ToLength::LENGTH)
}

pub fn r#typeof(vm: &Vm, pc: u32, values: &mut op::TypeofValues) -> SlowPathControl {
    values.dst = Value::from_string(values.src.typeof_(vm));
    SlowPathControl::continue_at(pc + op::Typeof::LENGTH)
}

pub fn postfix_decrement(vm: &Vm, pc: u32, values: &mut op::PostfixDecrementValues) -> SlowPathControl {
    let old_value = asm_try!(vm, pc, values.src.to_numeric(vm));
    values.dst = old_value;
    if old_value.is_number() {
        values.src = Value::from_f64(old_value.as_f64() - 1.0);
    } else {
        let result = old_value.as_bigint().big_integer() - 1;
        values.src = Value::from_bigint(BigInt::create(vm, result));
    }
    SlowPathControl::continue_at(pc + op::PostfixDecrement::LENGTH)
}

pub fn to_int32(vm: &Vm, pc: u32, values: &mut op::ToInt32Values) -> SlowPathControl {
    values.dst = Value::from_i32(asm_try!(vm, pc, values.value.to_i32(vm)));
    SlowPathControl::continue_at(pc + op::ToInt32::LENGTH)
}

pub fn bitwise_not(vm: &Vm, pc: u32, values: &mut op::BitwiseNotValues) -> SlowPathControl {
    values.dst = asm_try!(vm, pc, value::bitwise_not(vm, values.src));
    SlowPathControl::continue_at(pc + op::BitwiseNot::LENGTH)
}

pub fn unary_plus(vm: &Vm, pc: u32, values: &mut op::UnaryPlusValues) -> SlowPathControl {
    values.dst = asm_try!(vm, pc, value::unary_plus(vm, values.src));
    SlowPathControl::continue_at(pc + op::UnaryPlus::LENGTH)
}

pub fn is_constructor(pc: u32, values: &mut op::IsConstructorValues) -> SlowPathControl {
    values.dst = Value::from_bool(values.value.is_constructor());
    SlowPathControl::continue_at(pc + op::IsConstructor::LENGTH)
}

pub fn instance_of(vm: &Vm, pc: u32, values: &mut op::InstanceOfValues) -> SlowPathControl {
    let result = asm_try!(vm, pc, value::instance_of(vm, values.lhs, values.rhs));
    values.dst = result;
    SlowPathControl::continue_at(pc + op::InstanceOf::LENGTH)
}

pub fn r#in(vm: &Vm, pc: u32, values: &mut op::InValues) -> SlowPathControl {
    let result = asm_try!(vm, pc, value::r#in(vm, values.lhs, values.rhs));
    values.dst = result;
    SlowPathControl::continue_at(pc + op::In::LENGTH)
}

/// Converts a value to a boolean for the jump handlers: 0 for false and 1 for true. It never throws.
pub fn helper_to_boolean(encoded_value: u64) -> u64 {
    let value = Value(encoded_value);
    u64::from(value.to_boolean())
}

pub fn helper_math_exp(encoded_value: u64) -> u64 {
    let value = Value(encoded_value);
    Value::from_f64(value.as_f64().exp()).0
}

pub fn helper_empty_string(vm: &Vm) -> u64 {
    Value::from_string(vm.empty_string()).0
}

pub fn helper_single_ascii_character_string(vm: &Vm, encoded_value: u64) -> u64 {
    Value::from_string(vm.single_ascii_character_string(encoded_value as u8)).0
}

pub fn helper_single_utf16_code_unit_string(vm: &Vm, encoded_value: u64) -> u64 {
    let code_unit = encoded_value as u16;
    Value::from_string(PrimitiveString::create_from_utf16_view(
        vm,
        Utf16View::Utf16(&[code_unit]),
    ))
    .0
}
