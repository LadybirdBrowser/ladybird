/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The slow paths for arithmetic, comparisons and conversions, and their helpers.

use core::cell::Cell;

use crate::bytecode::executable::Executable;
use crate::bytecode::op;
use crate::interpreter::runtime_functions::{SlowPathControl, asm_try};
use crate::interpreter::vm::Vm;
use crate::layout::value::Value;
use crate::runtime::big_int::BigInt;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::value::{self, PreferredType};
use crate::utf16::Utf16View;

/// The size of the instruction at `pc` in the running executable. The slow paths that receive their operands in
/// registers serve several instructions, so they read it from the bytecode.
fn length_of_instruction_at(vm: &Vm, pc: u32) -> u32 {
    let context = vm.running_execution_context().expect("a slow path runs in a frame");
    // SAFETY: The running context is live.
    let executable = unsafe { context.as_ref() }
        .executable
        .get()
        .expect("a running frame has an executable");
    // SAFETY: Executables start with their head, and the running one is live.
    let bytecode = unsafe { executable.as_non_null().cast::<Executable>().as_ref() }.bytecode();
    let pc = pc as usize;
    let length = crate::bytecode::instruction::instruction_length_from_bytes(bytecode[pc], bytecode, pc)
        .unwrap_or_else(|error| panic!("the instruction at {pc} has no length: {error:?}"));
    u32::try_from(length).expect("an instruction's length fits in u32")
}

fn finish_binary_slow_path_value(vm: &Vm, pc: u32, destination: &Cell<Value>, result: Value) -> SlowPathControl {
    destination.set(result);
    SlowPathControl::continue_at(pc + length_of_instruction_at(vm, pc))
}

fn finish_binary_slow_path(
    vm: &Vm,
    pc: u32,
    destination: &Cell<Value>,
    result: ThrowCompletionOr<Value>,
) -> SlowPathControl {
    finish_binary_slow_path_value(vm, pc, destination, asm_try!(vm, pc, result))
}

fn finish_binary_slow_path_with_boolean(
    vm: &Vm,
    pc: u32,
    destination: &Cell<Value>,
    result: ThrowCompletionOr<bool>,
) -> SlowPathControl {
    finish_binary_slow_path_value(vm, pc, destination, Value::from_bool(asm_try!(vm, pc, result)))
}

pub fn add_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, destination, value::add(vm, lhs, rhs))
}

pub fn sub_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, destination, value::sub(vm, lhs, rhs))
}

pub fn mul_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, destination, value::mul(vm, lhs, rhs))
}

pub fn div_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, destination, value::div(vm, lhs, rhs))
}

pub fn less_than_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path_with_boolean(vm, pc, destination, value::less_than(vm, lhs, rhs))
}

pub fn less_than_equals_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path_with_boolean(vm, pc, destination, value::less_than_equals(vm, lhs, rhs))
}

pub fn greater_than_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path_with_boolean(vm, pc, destination, value::greater_than(vm, lhs, rhs))
}

pub fn greater_than_equals_values(
    vm: &Vm,
    pc: u32,
    destination: &Cell<Value>,
    lhs: Value,
    rhs: Value,
) -> SlowPathControl {
    finish_binary_slow_path_with_boolean(vm, pc, destination, value::greater_than_equals(vm, lhs, rhs))
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
    finish_binary_slow_path(vm, pc, destination, value::exp(vm, lhs, rhs))
}

pub fn bitwise_xor_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, destination, value::bitwise_xor(vm, lhs, rhs))
}

pub fn bitwise_and_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, destination, value::bitwise_and(vm, lhs, rhs))
}

pub fn bitwise_or_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, destination, value::bitwise_or(vm, lhs, rhs))
}

pub fn left_shift_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, destination, value::left_shift(vm, lhs, rhs))
}

pub fn right_shift_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, destination, value::right_shift(vm, lhs, rhs))
}

pub fn unsigned_right_shift_values(
    vm: &Vm,
    pc: u32,
    destination: &Cell<Value>,
    lhs: Value,
    rhs: Value,
) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, destination, value::unsigned_right_shift(vm, lhs, rhs))
}

pub fn mod_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path(vm, pc, destination, value::r#mod(vm, lhs, rhs))
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

pub fn strictly_equals_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path_value(vm, pc, destination, Value::from_bool(strictly_equals(lhs, rhs)))
}

pub fn strictly_inequals_values(
    vm: &Vm,
    pc: u32,
    destination: &Cell<Value>,
    lhs: Value,
    rhs: Value,
) -> SlowPathControl {
    finish_binary_slow_path_value(vm, pc, destination, Value::from_bool(!strictly_equals(lhs, rhs)))
}

pub fn loosely_equals_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path_with_boolean(vm, pc, destination, loosely_equals(vm, lhs, rhs))
}

pub fn loosely_inequals_values(vm: &Vm, pc: u32, destination: &Cell<Value>, lhs: Value, rhs: Value) -> SlowPathControl {
    finish_binary_slow_path_with_boolean(vm, pc, destination, loosely_inequals(vm, lhs, rhs))
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
