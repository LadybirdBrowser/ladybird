/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Text descriptions of the feedback the interpreter collected, with every instruction that has feedback slots and
//! what they recorded, for LIBJS_JIT=dump-feedback and the `jit` testing object.

use core::fmt::Write;

use crate::bytecode::executable::Executable;
use crate::bytecode::feedback::{
    CallFeedback, CallFeedbackForwarding, KeyedFeedback, arith_feedback, call_feedback_flags, keyed_feedback_bits,
    value_feedback,
};
use crate::bytecode::instruction::{
    instruction_feedback_slots_from_bytes, instruction_length_from_bytes, instruction_name_from_opcode,
};
use crate::gc::class::{GcCell, class_of};
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::function_object::NativeFunction;
use crate::layout::object::Object;
use crate::layout::primitive_string::PrimitiveString;
use crate::runtime::symbol::Symbol;
use crate::runtime::typed_array::Kind as TypedArrayKind;
use crate::utf16::Utf16View;

const TYPED_ARRAY_KINDS: [TypedArrayKind; 12] = [
    TypedArrayKind::Uint8Array,
    TypedArrayKind::Uint8ClampedArray,
    TypedArrayKind::Uint16Array,
    TypedArrayKind::Uint32Array,
    TypedArrayKind::BigUint64Array,
    TypedArrayKind::Int8Array,
    TypedArrayKind::Int16Array,
    TypedArrayKind::Int32Array,
    TypedArrayKind::BigInt64Array,
    TypedArrayKind::Float16Array,
    TypedArrayKind::Float32Array,
    TypedArrayKind::Float64Array,
];

/// The cell as a `T`, if it is one.
fn cell_as<T: GcCell>(cell: Gc<CellHeader>) -> Option<Gc<T>> {
    class_of(cell)
        .is_subclass_of(T::CLASS)
        // SAFETY: The cell was allocated as a T or a subclass of it.
        .then(|| unsafe { Gc::from_non_null(cell.as_non_null().cast()) })
}

fn append_bits<T: Copy + Into<u32>>(out: &mut String, bits: T, names: &[(T, &str)]) {
    let bits: u32 = bits.into();
    if bits == 0 {
        out.push_str("none");
        return;
    }
    let mut first = true;
    for (bit, name) in names {
        let bit: u32 = (*bit).into();
        if bits & bit == 0 {
            continue;
        }
        if !first {
            out.push('|');
        }
        out.push_str(name);
        first = false;
    }
}

fn append_cell(out: &mut String, cell: Option<Gc<CellHeader>>) {
    let Some(cell) = cell else {
        out.push_str("none");
        return;
    };
    if let Some(string) = cell_as::<PrimitiveString>(cell) {
        let _ = write!(out, "\"{}\"", string.to_utf8());
    } else if let Some(symbol) = cell_as::<Symbol>(cell) {
        out.push_str(&Utf16View::of_string(&symbol.descriptive_string()).to_utf8());
    } else if let Some(native_function) = cell_as::<NativeFunction>(cell) {
        let name = native_function.name();
        let name = match native_function.initial_name() {
            Some(initial_name) if name.is_empty() => initial_name,
            _ => name,
        };
        let name = Utf16View::of_fly_string(&name).to_utf8();
        if name.is_empty() {
            out.push_str("native function <anonymous>");
        } else {
            let _ = write!(out, "native function {name}");
        }
    } else if let Some(object) = cell_as::<Object>(cell)
        && object.is_function()
    {
        let name = Utf16View::of_string(&object.name_for_call_stack()).to_utf8();
        if name.is_empty() {
            out.push_str("function <anonymous>");
        } else {
            let _ = write!(out, "function {name}");
        }
    } else {
        out.push_str(class_of(cell).class_name());
    }
}

fn append_call_feedback(out: &mut String, feedback: &CallFeedback) {
    out.push_str("target=");
    append_cell(out, feedback.target());
    out.push_str(" flags=");
    append_bits(
        out,
        feedback.flags.get(),
        &[
            (call_feedback_flags::POLYMORPHIC, "Polymorphic"),
            (call_feedback_flags::SAW_NATIVE, "SawNative"),
            (call_feedback_flags::SAW_CONSTRUCT, "SawConstruct"),
            (call_feedback_flags::FORWARDED_POLYMORPHIC, "ForwardedPolymorphic"),
            (call_feedback_flags::FORWARDED_CLOSURES, "ForwardedClosures"),
            (call_feedback_flags::OTHER_FUNCTIONS, "OtherFunctions"),
        ],
    );
    let forwarding = match feedback.forwarding.get() {
        forwarding if forwarding == CallFeedbackForwarding::Apply as u8 => "apply",
        forwarding if forwarding == CallFeedbackForwarding::Call as u8 => "call",
        forwarding if forwarding == CallFeedbackForwarding::Bound as u8 => "bound",
        forwarding if forwarding == CallFeedbackForwarding::Callback as u8 => "callback",
        _ => return,
    };
    let _ = write!(
        out,
        " forwarded by {forwarding} with {} arguments to ",
        feedback.forwarded_argument_count.get()
    );
    append_cell(out, feedback.forwarded_target());
}

fn append_keyed_feedback(out: &mut String, feedback: &KeyedFeedback) {
    let bits = feedback.bits.get();
    out.push_str("keys=");
    append_bits(
        out,
        bits & keyed_feedback_bits::KEY_KINDS_MASK,
        &[
            (keyed_feedback_bits::INT32_INDEX, "Int32Index"),
            (keyed_feedback_bits::STRING_KEY, "String"),
            (keyed_feedback_bits::SYMBOL_KEY, "Symbol"),
            (keyed_feedback_bits::OTHER_KEY, "Other"),
            (keyed_feedback_bits::MULTIPLE_KEYS, "MultipleKeys"),
        ],
    );
    out.push_str(" elements=");
    let mut element_names = vec![
        (keyed_feedback_bits::PACKED, "Packed"),
        (keyed_feedback_bits::HOLEY, "Holey"),
        (keyed_feedback_bits::OTHER_ELEMENTS, "Other"),
    ];
    let typed_array_names = TYPED_ARRAY_KINDS.map(|kind| format!("{kind:?}"));
    for (kind, name) in TYPED_ARRAY_KINDS.iter().zip(&typed_array_names) {
        element_names.push((keyed_feedback_bits::typed_array_bit(*kind as u8), name.as_str()));
    }
    append_bits(
        out,
        bits & !keyed_feedback_bits::KEY_KINDS_MASK & !keyed_feedback_bits::OUT_OF_BOUNDS,
        &element_names,
    );
    if bits & keyed_feedback_bits::OUT_OF_BOUNDS != 0 {
        out.push_str(" out-of-bounds");
    }
    if feedback.last_key().is_some() {
        out.push_str(" last_key=");
        append_cell(out, feedback.last_key());
    }
}

/// The feedback of every instruction of `executable` that has feedback slots, one line per instruction after a line
/// naming the executable, or `None` if the executable never collected feedback.
pub fn describe_feedback(executable: &Executable) -> Option<String> {
    let feedback = executable.feedback()?;
    feedback.update_value_feedback();
    let name = executable.name();
    let name = Utf16View::of_fly_string(&name).to_utf8();
    let mut out = String::new();
    if name.is_empty() {
        out.push_str("Feedback for <anonymous>:\n");
    } else {
        let _ = writeln!(out, "Feedback for {name}:");
    }

    let bytecode = executable.bytecode();
    let mut at = 0;
    while at < bytecode.len() {
        let opcode = bytecode[at];
        let [arith, value, call, keyed] = instruction_feedback_slots_from_bytes(opcode, bytecode, at);
        if arith.is_some() || value.is_some() || call.is_some() || keyed.is_some() {
            let _ = write!(out, "  [{at:4x}] {}", instruction_name_from_opcode(opcode));
            if let Some(slot) = arith {
                let _ = write!(out, " arith#{slot}: ");
                append_bits(
                    &mut out,
                    feedback.arith()[usize::from(slot)].get(),
                    &[
                        (arith_feedback::INT32, "Int32"),
                        (arith_feedback::DOUBLE, "Double"),
                        (arith_feedback::INT32_OVERFLOW, "Int32Overflow"),
                        (arith_feedback::STRING, "String"),
                        (arith_feedback::BIG_INT, "BigInt"),
                        (arith_feedback::OTHER, "Other"),
                    ],
                );
            }
            if let Some(slot) = value {
                let _ = write!(out, " value#{slot}: ");
                append_bits(
                    &mut out,
                    feedback.value[usize::from(slot)].get(),
                    &[
                        (value_feedback::INT32, "Int32"),
                        (value_feedback::DOUBLE, "Double"),
                        (value_feedback::STRING, "String"),
                        (value_feedback::SYMBOL, "Symbol"),
                        (value_feedback::BOOLEAN, "Boolean"),
                        (value_feedback::UNDEFINED, "Undefined"),
                        (value_feedback::NULL, "Null"),
                        (value_feedback::OBJECT, "Object"),
                        (value_feedback::BIG_INT, "BigInt"),
                        (value_feedback::EMPTY, "Empty"),
                        (value_feedback::OTHER_MASK, "Other"),
                    ],
                );
            }
            if let Some(slot) = call {
                let _ = write!(out, " call#{slot}: ");
                append_call_feedback(&mut out, &feedback.call()[usize::from(slot)]);
            }
            if let Some(slot) = keyed {
                let _ = write!(out, " keyed#{slot}: ");
                append_keyed_feedback(&mut out, &feedback.keyed()[usize::from(slot)]);
            }
            out.push('\n');
        }
        let Ok(length) = instruction_length_from_bytes(opcode, bytecode, at) else {
            break;
        };
        at += length;
    }
    Some(out)
}
