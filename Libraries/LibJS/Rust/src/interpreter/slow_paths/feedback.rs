/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Slow paths record feedback for the optimizing JIT the way the profiling interpreter's fast paths do, while the
//! interpreter collects feedback, for the executables that left the plain tier.

use crate::bytecode::executable::Executable;
use crate::bytecode::feedback::{
    ExecutableFeedback, are_closures_of_one_function, call_feedback_flags, keyed_feedback_bits,
};
use crate::bytecode::instruction::OpCode;
use crate::bytecode::op;
use crate::interpreter::runtime_functions::SlowPathControl;
use crate::interpreter::vm::Vm;
use crate::layout::cell::CellHeader;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::feedback::{CallFeedbackForwarding, KeyedFeedback};
use crate::layout::object::IndexedStorageKind;
use crate::layout::value::Value;
use crate::runtime::byte_length::ByteLength;
use crate::runtime::typed_array::TypedArrayBase;

/// Calls `record` with the feedback of the running executable, if the interpreter collects it.
fn record_for_running_executable(vm: &Vm, record: impl FnOnce(&ExecutableFeedback)) {
    if !vm.jit.collects_feedback() {
        return;
    }
    if let Some(feedback) = vm.current_executable().feedback() {
        record(feedback);
    }
}

/// Stores `value` in the bucket of value feedback slot `slot` of the running executable.
pub fn record_value(vm: &Vm, slot: u16, value: Value) {
    record_for_running_executable(vm, |feedback| {
        feedback.value_buckets()[usize::from(slot)].set(value.0);
    });
}

/// Records the value a slow path produced, if it continues in the frame it was called from, and passes its control
/// word on.
pub fn record_value_after(
    vm: &Vm,
    control: SlowPathControl,
    slot: u16,
    value: impl FnOnce() -> Value,
) -> SlowPathControl {
    if control.continues_in_frame() {
        record_value(vm, slot, value());
    }
    control
}

/// Records a callee of the call site with call feedback slot `slot`, and `flags`.
pub fn record_call(vm: &Vm, slot: u16, callee: Value, mut flags: u8) {
    record_for_running_executable(vm, |feedback| {
        let call = &feedback.call()[usize::from(slot)];
        if callee.is_cell() {
            let target = callee.as_cell();
            if call.target() != Some(target) {
                match call.target() {
                    None => call.target.set(Some(target)),
                    Some(first) => {
                        flags |= call_feedback_flags::POLYMORPHIC;
                        if !are_closures_of_one_function(first, target) {
                            flags |= call_feedback_flags::OTHER_FUNCTIONS;
                        }
                    }
                }
                if !callee.is_object() || !callee.as_object().is_ecmascript_function_object() {
                    flags |= call_feedback_flags::SAW_NATIVE;
                }
            }
        }
        call.flags.set(call.flags.get() | flags);
    });
}

/// ORs `bits` into keyed feedback slot `slot`.
pub fn record_keyed_bits(vm: &Vm, slot: u16, bits: u32) {
    record_for_running_executable(vm, |feedback| {
        let keyed = &feedback.keyed()[usize::from(slot)];
        keyed.bits.set(keyed.bits.get() | bits);
    });
}

/// Records the key and element kinds of a keyed access of `property` on `base`.
pub fn record_keyed(vm: &Vm, slot: u16, base: Value, property: Value) {
    record_for_running_executable(vm, |feedback| {
        let keyed = &feedback.keyed()[usize::from(slot)];
        keyed.bits.set(keyed.bits.get() | keyed_bits(keyed, base, property));
    });
}

/// The keyed feedback bits of an access of `property` on `base`, noting a string or symbol key as the last key.
fn keyed_bits(keyed: &KeyedFeedback, base: Value, property: Value) -> u32 {
    if property.is_int32() {
        let Ok(index) = u32::try_from(property.as_i32()) else {
            return keyed_feedback_bits::OTHER_KEY | keyed_feedback_bits::OTHER_ELEMENTS;
        };
        if !base.is_object() {
            return keyed_feedback_bits::OTHER_KEY | keyed_feedback_bits::OTHER_ELEMENTS;
        }
        let object = base.as_object();
        let mut bits = keyed_feedback_bits::INT32_INDEX;
        if let Some(typed_array) = object.downcast::<TypedArrayBase>() {
            bits |= keyed_feedback_bits::typed_array_bit(typed_array.kind() as u8);
            match typed_array.array_length() {
                ByteLength::Length(length) if index < length => {}
                _ => bits |= keyed_feedback_bits::OUT_OF_BOUNDS,
            }
        } else {
            bits |= match object.indexed_storage_kind() {
                IndexedStorageKind::Packed => keyed_feedback_bits::PACKED,
                IndexedStorageKind::Holey => keyed_feedback_bits::HOLEY,
                _ => keyed_feedback_bits::OTHER_ELEMENTS,
            };
            // NB: Stores that fill holes or grow the storage, and loads of holes, are out of bounds.
            if !object.has_stored_indexed_element(index) {
                bits |= keyed_feedback_bits::OUT_OF_BOUNDS;
            }
        }
        return bits;
    }

    let mut bits = keyed_feedback_bits::OTHER_ELEMENTS;
    if property.is_string() || property.is_symbol() {
        bits |= if property.is_string() {
            keyed_feedback_bits::STRING_KEY
        } else {
            keyed_feedback_bits::SYMBOL_KEY
        };
        let key = property.as_cell();
        if keyed.last_key() != Some(key) {
            if keyed.last_key().is_some() {
                bits |= keyed_feedback_bits::MULTIPLE_KEYS;
            }
            keyed.last_key.set(Some(key));
        }
    } else {
        bits |= keyed_feedback_bits::OTHER_KEY;
    }
    bits
}

/// Records in the call feedback of the Call instruction at `pc` of `frame`, whose first callee is `callee`, that the
/// callee forwarded the call to `target`, while the interpreter collects feedback.
pub fn record_forwarded_call(
    vm: &Vm,
    frame: &ExecutionContext,
    pc: u32,
    callee: Gc<CellHeader>,
    target: Gc<CellHeader>,
    forwarding: CallFeedbackForwarding,
    argument_count: usize,
) {
    if !vm.jit.collects_feedback() {
        return;
    }
    let Some(executable) = frame.executable.get() else {
        return;
    };
    let executable = Executable::from_head(executable);
    let Some(feedback) = executable.feedback() else {
        return;
    };
    let bytecode = executable.bytecode();
    if pc as usize >= bytecode.len() || bytecode[pc as usize] != OpCode::Call as u8 {
        return;
    }
    // SAFETY: The bytes at pc are a Call instruction.
    let call = unsafe { &*bytecode[pc as usize..].as_ptr().cast::<op::Call>() };
    let call_feedback = &feedback.call()[usize::from(call.call_feedback)];
    if call_feedback.target() != Some(callee) {
        return;
    }
    call_feedback.record_forwarded_call(target, forwarding, argument_count);
}

/// record_forwarded_call() for the call that the running frame, the frame the interpreter's call fast path built for a
/// raw native function, runs for its caller.
pub fn record_forwarded_call_from_native(
    vm: &Vm,
    target: Gc<CellHeader>,
    forwarding: CallFeedbackForwarding,
    argument_count: usize,
) {
    if !vm.jit.collects_feedback() {
        return;
    }
    let native_frame = vm.running_execution_context_ref();
    // NB: The interpreter's call fast path runs raw native functions in a frame without an executable, linked to the
    //     caller at its Call.
    let caller = native_frame.caller_frame.get();
    if native_frame.executable.get().is_some() || caller.is_null() {
        return;
    }
    let Some(function) = native_frame.function.get() else {
        return;
    };
    // SAFETY: A frame's caller outlives it.
    let caller = unsafe { &*caller };
    // SAFETY: Every cell starts with a cell header.
    let function = unsafe { Gc::<CellHeader>::from_non_null(function.as_non_null().cast()) };
    record_forwarded_call(
        vm,
        caller,
        caller.program_counter.get(),
        function,
        target,
        forwarding,
        argument_count,
    );
}
