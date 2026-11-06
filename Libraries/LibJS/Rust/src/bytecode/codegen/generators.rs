/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Yield, await and yield*, and disposing of resources, which can take awaiting.

use libjs_abi::CompletionType;
use libjs_abi::IteratorHint;

use crate::bytecode::instruction::typed as op;
use crate::bytecode::operand::*;
use crate::utf16::Utf16View;

use super::context::Generator;
use super::registers::ScopedOperand;
use super::stream::Exit;

#[derive(Clone, Copy, PartialEq, Eq)]
enum YieldBehavior {
    Normal,
    AwaitValue,
    IteratorResult,
}

/// The register that a resumed function finds what it was resumed with in.
#[inline(always)]
fn accumulator(generator: &mut Generator<'_>) -> ScopedOperand {
    generator.scoped_operand(Operand::register(Register::ACCUMULATOR))
}

/// Emit a property access by a name of the iteration protocol, which is not "length".
fn emit_get_by_known_name(
    generator: &mut Generator<'_>,
    dst: &ScopedOperand,
    base: &ScopedOperand,
    name: Utf16View<'_>,
) {
    let key = generator.intern_property_key_text(name);
    let cache = generator.next_property_lookup_cache();
    let value_feedback = generator.next_value_feedback();
    generator.emit(op::GetById {
        value_feedback,
        dst: dst.operand(),
        base: base.operand(),
        property: key,
        base_identifier: None,
        cache,
    });
}

/// Generate bytecode that awaits `value`.
pub(super) fn generate_await_of_value(generator: &mut Generator<'_>, value: &ScopedOperand) -> ScopedOperand {
    // Allocate received_completion registers before the await.
    let received_completion = generator.allocate_register();
    let received_completion_type = generator.allocate_register();
    let received_completion_value = generator.allocate_register();
    let acc = accumulator(generator);
    generator.emit_mov(&received_completion, &acc);
    generate_await_with_completions(
        generator,
        value,
        &received_completion,
        &received_completion_type,
        &received_completion_value,
    )
}

/// Generate bytecode that yields `value`, and goes on with what the generator gets resumed with.
pub(super) fn generate_yield_of_value(
    generator: &mut Generator<'_>,
    value: ScopedOperand,
    is_yield_from: bool,
    received_completion: &ScopedOperand,
    received_completion_type: &ScopedOperand,
    received_completion_value: &ScopedOperand,
) -> ScopedOperand {
    if is_yield_from {
        return generate_yield_from(
            generator,
            value,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
    }

    // Create continuation block, call generate_yield, then handle
    // completion checking.
    let continuation_block = generator.new_label();

    generate_yield(
        generator,
        continuation_block,
        &value,
        received_completion,
        received_completion_type,
        received_completion_value,
        if generator.is_in_async_generator_function() {
            YieldBehavior::AwaitValue
        } else {
            YieldBehavior::Normal
        },
    );

    generator.bind_reachable_label(continuation_block);

    // Restore exception register after resuming.

    let acc = accumulator(generator);
    generator.emit_mov(received_completion, &acc);
    generator.emit(op::GetCompletionFields {
        type_dst: received_completion_type.operand(),
        value_dst: received_completion_value.operand(),
        completion: received_completion.operand(),
    });

    let normal_block = generator.new_label();
    let throw_cont = generator.new_label();
    let type_is_normal = generator.allocate_register();
    let normal_type = generator.add_constant_number(f64::from(CompletionType::Normal as u32));
    let arith_feedback = generator.next_arith_feedback();
    generator.emit(op::StrictlyEquals {
        arith_feedback,
        dst: type_is_normal.operand(),
        lhs: received_completion_type.operand(),
        rhs: normal_type.operand(),
    });
    generator.emit_jump_if(&type_is_normal, normal_block, throw_cont);

    let throw_value_block = generator.new_label();
    let return_value_block = generator.new_label();

    generator.bind_reachable_label(throw_cont);
    let type_is_throw = generator.allocate_register();
    let throw_type = generator.add_constant_number(f64::from(CompletionType::Throw as u32));
    let arith_feedback = generator.next_arith_feedback();
    generator.emit(op::StrictlyEquals {
        arith_feedback,
        dst: type_is_throw.operand(),
        lhs: received_completion_type.operand(),
        rhs: throw_type.operand(),
    });
    generator.emit_jump_if(&type_is_throw, throw_value_block, return_value_block);

    generator.bind_reachable_label(throw_value_block);
    generator.emit(op::Throw {
        src: received_completion_value.operand(),
    });

    generator.bind_reachable_label(return_value_block);
    generator.emit_exit(Exit::Return, received_completion_value.operand());

    generator.bind_reachable_label(normal_block);
    received_completion_value.clone()
}

/// Like generate_await but uses caller-provided completion registers.
///
/// Returns the received_completion_value on the normal path.
/// Emits a Throw on the throw path.
pub(super) fn generate_await_with_completions(
    generator: &mut Generator<'_>,
    argument: &ScopedOperand,
    received_completion: &ScopedOperand,
    received_completion_type: &ScopedOperand,
    received_completion_value: &ScopedOperand,
) -> ScopedOperand {
    let continuation = generator.new_label();
    generator.emit(op::Await {
        continuation_label: continuation,
        argument: argument.operand(),
    });
    generator.bind_reachable_label(continuation);

    let acc = accumulator(generator);
    generator.emit_mov(received_completion, &acc);
    generator.emit(op::GetCompletionFields {
        type_dst: received_completion_type.operand(),
        value_dst: received_completion_value.operand(),
        completion: received_completion.operand(),
    });

    let normal_block = generator.new_label();
    let throw_block = generator.new_label();
    let is_normal = generator.allocate_register();
    let normal_type = generator.add_constant_number(f64::from(CompletionType::Normal as u32));
    let arith_feedback = generator.next_arith_feedback();
    generator.emit(op::StrictlyEquals {
        arith_feedback,
        dst: is_normal.operand(),
        lhs: received_completion_type.operand(),
        rhs: normal_type.operand(),
    });
    generator.emit_jump_if(&is_normal, normal_block, throw_block);

    generator.bind_reachable_label(throw_block);
    generator.emit(op::Throw {
        src: received_completion_value.operand(),
    });

    generator.bind_reachable_label(normal_block);
    received_completion_value.clone()
}

/// Yield* (yield from) delegation.
///
/// Implements the iterator delegation protocol from
/// https://tc39.es/ecma262/#sec-generator-function-definitions-runtime-semantics-evaluation
///
/// The delegating generator forwards next/throw/return to the inner iterator.
fn generate_yield_from(
    generator: &mut Generator<'_>,
    value: ScopedOperand,
    received_completion: &ScopedOperand,
    received_completion_type: &ScopedOperand,
    received_completion_value: &ScopedOperand,
) -> ScopedOperand {
    let is_async = generator.is_in_async_generator_function();

    // 4. Let iteratorRecord be ? GetIterator(value, generatorKind).
    let iterator = generator.allocate_register();
    let next_method = generator.allocate_register();
    let iterator_done_property = generator.allocate_register();
    let hint = if is_async {
        IteratorHint::Async
    } else {
        IteratorHint::Sync
    } as u32;
    let cache = generator.next_property_lookup_cache();
    generator.emit(op::GetIterator {
        dst_iterator_object: iterator.operand(),
        dst_iterator_next: next_method.operand(),
        dst_iterator_done: iterator_done_property.operand(),
        iterable: value.operand(),
        hint,
        // NB: yield* passes the iterator to calls of its methods.
        fast_array_mode_allowed: false,
        cache,
    });

    // 6. Let received be NormalCompletion(undefined).
    let normal_const = generator.add_constant_number(f64::from(CompletionType::Normal as u32));
    generator.emit_mov(received_completion_type, &normal_const);
    let undef = generator.add_constant_undefined();
    generator.emit_mov(received_completion_value, &undef);

    // 7. Repeat,
    let loop_block = generator.new_label();
    let continuation_block = generator.new_label();
    let loop_end_block = generator.new_label();
    generator.emit(op::Jump { target: loop_block });
    generator.bind_reachable_label(loop_block);

    // Branch on received.[[Type]].
    let type_is_normal_block = generator.new_label();
    let is_type_throw_block = generator.new_label();
    let is_normal = generator.allocate_register();
    let arith_feedback = generator.next_arith_feedback();
    generator.emit(op::StrictlyEquals {
        arith_feedback,
        dst: is_normal.operand(),
        lhs: received_completion_type.operand(),
        rhs: normal_const.operand(),
    });
    generator.emit_jump_if(&is_normal, type_is_normal_block, is_type_throw_block);

    // =========================================================================
    // a. If received.[[Type]] is normal, then
    // =========================================================================
    generator.bind_reachable_label(type_is_normal_block);

    // i. Let _innerResult_ be ? Call(_next_, _iterator_, « _received_.[[Value]] »).
    let inner_result = generator.allocate_register();
    let value_feedback = generator.next_value_feedback();
    let call_feedback = generator.next_call_feedback();
    generator.emit(op::Call {
        value_feedback,
        call_feedback,
        dst: inner_result.operand(),
        callee: next_method.operand(),
        this_value: iterator.operand(),
        argument_count: 1,
        expression_string: None,
        arguments: &[received_completion_value.operand()],
    });

    // ii. If _generatorKind_ is ~async~, set _innerResult_ to ? Await(_innerResult_).
    if is_async {
        let awaited = generate_await_with_completions(
            generator,
            &inner_result,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
        generator.emit_mov(&inner_result, &awaited);
    }

    // iii. If _innerResult_ is not an Object, throw a *TypeError* exception.
    generator.emit(op::ThrowIfNotObject {
        src: inner_result.operand(),
    });

    // iv. Let _done_ be ? IteratorComplete(_innerResult_).
    let done = generator.allocate_register();
    emit_get_by_known_name(generator, &done, &inner_result, utf16!("done").into());

    // v. If _done_ is *true*, then return ? IteratorValue(_innerResult_).
    let type_is_normal_done_block = generator.new_label();
    let type_is_normal_not_done_block = generator.new_label();
    generator.emit_jump_if(&done, type_is_normal_done_block, type_is_normal_not_done_block);

    generator.bind_reachable_label(type_is_normal_done_block);
    let return_value = generator.allocate_register();
    emit_get_by_known_name(generator, &return_value, &inner_result, utf16!("value").into());
    generator.emit(op::Jump { target: loop_end_block });

    // vi. If _generatorKind_ is ~async~, set _received_ to Completion(AsyncGeneratorYield(? IteratorValue(_innerResult_))).
    // vii. Else, set _received_ to Completion(GeneratorYield(_innerResult_)).
    generator.bind_reachable_label(type_is_normal_not_done_block);
    {
        let (yield_value, yield_behavior) = if is_async {
            let current_value = generator.allocate_register();
            emit_get_by_known_name(generator, &current_value, &inner_result, utf16!("value").into());
            (current_value, YieldBehavior::Normal)
        } else {
            (inner_result.clone(), YieldBehavior::IteratorResult)
        };

        generate_yield(
            generator,
            continuation_block,
            &yield_value,
            received_completion,
            received_completion_type,
            received_completion_value,
            yield_behavior,
        );
    }

    // =========================================================================
    // b. Else if received.[[Type]] is throw, then
    // =========================================================================
    generator.bind_reachable_label(is_type_throw_block);
    let type_is_throw_block = generator.new_label();
    let type_is_return_block = generator.new_label();
    let throw_const = generator.add_constant_number(f64::from(CompletionType::Throw as u32));
    let is_throw = generator.allocate_register();
    let arith_feedback = generator.next_arith_feedback();
    generator.emit(op::StrictlyEquals {
        arith_feedback,
        dst: is_throw.operand(),
        lhs: received_completion_type.operand(),
        rhs: throw_const.operand(),
    });
    generator.emit_jump_if(&is_throw, type_is_throw_block, type_is_return_block);

    generator.bind_reachable_label(type_is_throw_block);

    // i. Let _throw_ be ? GetMethod(_iterator_, *"throw"*).
    let throw_method = generator.allocate_register();
    let throw_key = generator.intern_property_key_text(utf16!("throw").into());
    let cache = generator.next_property_lookup_cache();
    generator.emit(op::GetMethod {
        dst: throw_method.operand(),
        object: iterator.operand(),
        property: throw_key,
        cache,
    });

    // ii. If _throw_ is not *undefined*, then
    let throw_method_defined_block = generator.new_label();
    let throw_method_undefined_block = generator.new_label();
    generator.emit(op::JumpUndefined {
        condition: throw_method.operand(),
        true_target: throw_method_undefined_block,
        false_target: throw_method_defined_block,
    });

    generator.bind_reachable_label(throw_method_defined_block);

    // 1. Let _innerResult_ be ? Call(_throw_, _iterator_, « _received_.[[Value]] »).
    let value_feedback = generator.next_value_feedback();
    let call_feedback = generator.next_call_feedback();
    generator.emit(op::Call {
        value_feedback,
        call_feedback,
        dst: inner_result.operand(),
        callee: throw_method.operand(),
        this_value: iterator.operand(),
        argument_count: 1,
        expression_string: None,
        arguments: &[received_completion_value.operand()],
    });

    // 2. If _generatorKind_ is ~async~, set _innerResult_ to ? Await(_innerResult_).
    if is_async {
        let awaited = generate_await_with_completions(
            generator,
            &inner_result,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
        generator.emit_mov(&inner_result, &awaited);
    }

    // 4. If _innerResult_ is not an Object, throw a *TypeError* exception.
    generator.emit(op::ThrowIfNotObject {
        src: inner_result.operand(),
    });

    // 5. Let _done_ be ? IteratorComplete(_innerResult_).
    emit_get_by_known_name(generator, &done, &inner_result, utf16!("done").into());

    // 6. If _done_ is *true*, return ? IteratorValue(_innerResult_).
    let type_is_throw_done_block = generator.new_label();
    let type_is_throw_not_done_block = generator.new_label();
    generator.emit_jump_if(&done, type_is_throw_done_block, type_is_throw_not_done_block);

    generator.bind_reachable_label(type_is_throw_done_block);
    emit_get_by_known_name(generator, &return_value, &inner_result, utf16!("value").into());
    generator.emit(op::Jump { target: loop_end_block });

    // 7. If _generatorKind_ is ~async~, set _received_ to Completion(AsyncGeneratorYield(? IteratorValue(_innerResult_))).
    // 8. Else, set _received_ to Completion(GeneratorYield(_innerResult_)).
    generator.bind_reachable_label(type_is_throw_not_done_block);
    {
        let (yield_value, yield_behavior) = if is_async {
            let current_value = generator.allocate_register();
            emit_get_by_known_name(generator, &current_value, &inner_result, utf16!("value").into());
            (current_value, YieldBehavior::Normal)
        } else {
            (inner_result.clone(), YieldBehavior::IteratorResult)
        };
        generate_yield(
            generator,
            continuation_block,
            &yield_value,
            received_completion,
            received_completion_type,
            received_completion_value,
            yield_behavior,
        );
    }

    // throw is undefined: close iterator, throw TypeError.
    generator.bind_reachable_label(throw_method_undefined_block);

    if is_async {
        // AsyncIteratorClose: get return method, call it, await, check object.
        let return_method = generator.allocate_register();
        let return_key = generator.intern_property_key_text(utf16!("return").into());
        let cache = generator.next_property_lookup_cache();
        generator.emit(op::GetMethod {
            dst: return_method.operand(),
            object: iterator.operand(),
            property: return_key,
            cache,
        });

        let call_return_block = generator.new_label();
        let after_close = generator.new_label();
        generator.emit(op::JumpUndefined {
            condition: return_method.operand(),
            true_target: after_close,
            false_target: call_return_block,
        });
        generator.bind_reachable_label(call_return_block);

        let close_result = generator.allocate_register();
        let value_feedback = generator.next_value_feedback();
        let call_feedback = generator.next_call_feedback();
        generator.emit(op::Call {
            value_feedback,
            call_feedback,
            dst: close_result.operand(),
            callee: return_method.operand(),
            this_value: iterator.operand(),
            argument_count: 0,
            expression_string: None,
            arguments: &[],
        });

        let awaited = generate_await_with_completions(
            generator,
            &close_result,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
        generator.emit(op::ThrowIfNotObject { src: awaited.operand() });

        generator.emit(op::Jump { target: after_close });
        generator.bind_reachable_label(after_close);
    } else {
        // Sync: IteratorClose with Normal completion.
        let undef = generator.add_constant_undefined();
        let cache = generator.next_property_lookup_cache();
        generator.emit(op::IteratorClose {
            iterator_object: iterator.operand(),
            iterator_next: next_method.operand(),
            iterator_done: iterator_done_property.operand(),
            completion_type: CompletionType::Normal as u32,
            completion_value: undef.operand(),
            cache,
        });
    }

    // Throw a TypeError: iterator does not have a throw method.
    let exception = generator.allocate_register();
    let error_string =
        generator.intern_string_text(utf16!("yield* protocol violation: iterator must have a throw method").into());
    generator.emit(op::NewTypeError {
        dst: exception.operand(),
        error_string,
    });
    generator.emit(op::Throw {
        src: exception.operand(),
    });

    // =========================================================================
    // c. Else (received.[[Type]] is return)
    // =========================================================================
    generator.bind_reachable_label(type_is_return_block);

    // ii. Let _return_ be ? GetMethod(_iterator_, *"return"*).
    let return_method = generator.allocate_register();
    let return_key = generator.intern_property_key_text(utf16!("return").into());
    let cache = generator.next_property_lookup_cache();
    generator.emit(op::GetMethod {
        dst: return_method.operand(),
        object: iterator.operand(),
        property: return_key,
        cache,
    });

    // iii. If _return_ is *undefined*, then
    let return_is_undefined_block = generator.new_label();
    let return_is_defined_block = generator.new_label();
    generator.emit(op::JumpUndefined {
        condition: return_method.operand(),
        true_target: return_is_undefined_block,
        false_target: return_is_defined_block,
    });

    generator.bind_reachable_label(return_is_undefined_block);
    // 1. If _generatorKind_ is ~async~, set _received_.[[Value]] to ? Await(_received_.[[Value]]).
    if is_async {
        generate_await_with_completions(
            generator,
            received_completion_value,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
    }
    // 2. Return _received_.
    generator.emit_exit(Exit::Return, received_completion_value.operand());

    generator.bind_reachable_label(return_is_defined_block);

    // iv. Let _innerReturnResult_ be ? Call(_return_, _iterator_, « _received_.[[Value]] »).
    let inner_return_result = generator.allocate_register();
    let value_feedback = generator.next_value_feedback();
    let call_feedback = generator.next_call_feedback();
    generator.emit(op::Call {
        value_feedback,
        call_feedback,
        dst: inner_return_result.operand(),
        callee: return_method.operand(),
        this_value: iterator.operand(),
        argument_count: 1,
        expression_string: None,
        arguments: &[received_completion_value.operand()],
    });

    // v. If _generatorKind_ is ~async~, set _innerReturnResult_ to ? Await(_innerReturnResult_).
    if is_async {
        let awaited = generate_await_with_completions(
            generator,
            &inner_return_result,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
        generator.emit_mov(&inner_return_result, &awaited);
    }

    // vi. If _innerReturnResult_ is not an Object, throw a *TypeError* exception.
    generator.emit(op::ThrowIfNotObject {
        src: inner_return_result.operand(),
    });

    // vii. Let _done_ be ? IteratorComplete(_innerReturnResult_).
    emit_get_by_known_name(generator, &done, &inner_return_result, utf16!("done").into());

    // viii. If _done_ is *true*, then
    let type_is_return_done_block = generator.new_label();
    let type_is_return_not_done_block = generator.new_label();
    generator.emit_jump_if(&done, type_is_return_done_block, type_is_return_not_done_block);

    generator.bind_reachable_label(type_is_return_done_block);
    let inner_return_result_value = generator.allocate_register();
    emit_get_by_known_name(
        generator,
        &inner_return_result_value,
        &inner_return_result,
        utf16!("value").into(),
    );
    generator.emit_exit(Exit::Return, inner_return_result_value.operand());

    // ix. If _generatorKind_ is ~async~, set _received_ to Completion(AsyncGeneratorYield(? IteratorValue(_innerReturnResult_))).
    // x. Else, set _received_ to Completion(GeneratorYield(_innerReturnResult_)).
    generator.bind_reachable_label(type_is_return_not_done_block);
    let (received, yield_behavior) = if is_async {
        let current_value = generator.allocate_register();
        emit_get_by_known_name(generator, &current_value, &inner_return_result, utf16!("value").into());
        (current_value, YieldBehavior::Normal)
    } else {
        (inner_return_result.clone(), YieldBehavior::IteratorResult)
    };
    generate_yield(
        generator,
        continuation_block,
        &received,
        received_completion,
        received_completion_type,
        received_completion_value,
        yield_behavior,
    );

    // =========================================================================
    // Continuation block: resume after any yield, extract completion, loop back.
    // =========================================================================
    generator.bind_reachable_label(continuation_block);
    let acc = accumulator(generator);
    generator.emit_mov(received_completion, &acc);
    generator.emit(op::GetCompletionFields {
        type_dst: received_completion_type.operand(),
        value_dst: received_completion_value.operand(),
        completion: received_completion.operand(),
    });
    generator.emit(op::Jump { target: loop_block });

    // =========================================================================
    // Loop end: return the accumulated return_value.
    // =========================================================================
    generator.bind_reachable_label(loop_end_block);
    return_value
}

/// Unified yield function.
///
/// For non-async generators: just emits a Yield instruction.
/// For async generators: optionally awaits the argument first, then yields,
/// then handles AsyncGeneratorUnwrapYieldResumption (check return type,
/// await return value, re-classify).
/// Jumps to continuation_label for the "not return" and "throw after await" paths.
fn generate_yield(
    generator: &mut Generator<'_>,
    continuation_label: Label,
    argument: &ScopedOperand,
    received_completion: &ScopedOperand,
    received_completion_type: &ScopedOperand,
    received_completion_value: &ScopedOperand,
    yield_behavior: YieldBehavior,
) {
    if !generator.is_in_async_generator_function() {
        if yield_behavior == YieldBehavior::IteratorResult {
            generator.emit(op::YieldIteratorResult {
                continuation_label,
                value: argument.operand(),
            });
        } else {
            generator.emit(op::Yield {
                continuation_label: Some(continuation_label),
                value: argument.operand(),
            });
        }
        return;
    }

    let argument = if yield_behavior == YieldBehavior::AwaitValue {
        generate_await_with_completions(
            generator,
            argument,
            received_completion,
            received_completion_type,
            received_completion_value,
        )
    } else {
        argument.clone()
    };

    // Yield, then UnwrapYieldResumption.
    let unwrap_block = generator.new_label();
    generator.emit(op::Yield {
        continuation_label: Some(unwrap_block),
        value: argument.operand(),
    });
    generator.bind_reachable_label(unwrap_block);

    let acc = accumulator(generator);
    generator.emit_mov(received_completion, &acc);
    generator.emit(op::GetCompletionFields {
        type_dst: received_completion_type.operand(),
        value_dst: received_completion_value.operand(),
        completion: received_completion.operand(),
    });

    // If resumptionValue.[[Type]] is not return, jump to continuation.
    let return_block = generator.new_label();
    let is_not_return = generator.allocate_register();
    let return_type = generator.add_constant_number(f64::from(CompletionType::Return as u32));
    let arith_feedback = generator.next_arith_feedback();
    generator.emit(op::StrictlyInequals {
        arith_feedback,
        dst: is_not_return.operand(),
        lhs: received_completion_type.operand(),
        rhs: return_type.operand(),
    });
    generator.emit_jump_if(&is_not_return, continuation_label, return_block);

    // Return path: Await(resumptionValue.[[Value]]).
    generator.bind_reachable_label(return_block);
    generate_await_with_completions(
        generator,
        received_completion_value,
        received_completion,
        received_completion_type,
        received_completion_value,
    );

    // If awaited.[[Type]] is throw, jump to continuation.
    let awaited_normal_block = generator.new_label();
    let is_throw = generator.allocate_register();
    let throw_type = generator.add_constant_number(f64::from(CompletionType::Throw as u32));
    let arith_feedback = generator.next_arith_feedback();
    generator.emit(op::StrictlyEquals {
        arith_feedback,
        dst: is_throw.operand(),
        lhs: received_completion_type.operand(),
        rhs: throw_type.operand(),
    });
    generator.emit_jump_if(&is_throw, continuation_label, awaited_normal_block);

    // awaited.[[Type]] is normal: set type to Return and jump to continuation.
    generator.bind_reachable_label(awaited_normal_block);
    generator.emit(op::SetCompletionType {
        completion: received_completion.operand(),
        completion_type: CompletionType::Return as u32,
    });
    generator.emit(op::Jump {
        target: continuation_label,
    });
}

/// How the resources of a scope are disposed of, if the scope has `using` or `await using` declarations.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum DisposalKind {
    Sync,
    Async,
}

/// Disposes of the resources in `stack`, which replaces the completion with a throw completion if a dispose method throws.
// https://tc39.es/ecma262/#sec-disposeresources
pub(super) fn emit_dispose_resources(
    generator: &mut Generator<'_>,
    kind: DisposalKind,
    stack: &ScopedOperand,
    completion_type: &ScopedOperand,
    completion_value: &ScopedOperand,
) {
    match kind {
        DisposalKind::Sync => {
            generator.emit(op::DisposeResources {
                stack: stack.operand(),
                completion_type: completion_type.operand(),
                completion_value: completion_value.operand(),
            });
        }
        DisposalKind::Async => {
            // NB: DisposeResources awaits between some of its steps here, so the bytecode performs it one step at a
            //     time, with an Await after each step that asks for one.
            let await_completion = generator.allocate_register();
            let await_completion_type = generator.allocate_register();
            let await_completion_value = generator.allocate_register();
            let done = generator.allocate_register();
            let awaited_value = generator.allocate_register();
            let normal_type = generator.add_constant_number(f64::from(CompletionType::Normal as u32));
            let undefined = generator.add_constant_undefined();
            generator.emit_mov(&await_completion_type, &normal_type);
            generator.emit_mov(&await_completion_value, &undefined);

            let step_block = generator.new_label();
            let await_block = generator.new_label();
            let continuation_block = generator.new_label();
            let done_block = generator.new_label();
            generator.emit(op::Jump { target: step_block });

            generator.bind_reachable_label(step_block);
            generator.emit(op::AsyncDisposeResourcesStep {
                done: done.operand(),
                awaited_value: awaited_value.operand(),
                stack: stack.operand(),
                completion_type: completion_type.operand(),
                completion_value: completion_value.operand(),
                await_completion_type: await_completion_type.operand(),
                await_completion_value: await_completion_value.operand(),
            });
            generator.emit_jump_if(&done, done_block, await_block);

            generator.bind_reachable_label(await_block);
            let acc = generator.scoped_operand(Operand::register(Register::ACCUMULATOR));
            generator.emit_mov(&await_completion, &acc);
            generator.emit(op::Await {
                continuation_label: continuation_block,
                argument: awaited_value.operand(),
            });

            generator.bind_reachable_label(continuation_block);
            let acc = generator.scoped_operand(Operand::register(Register::ACCUMULATOR));
            generator.emit_mov(&await_completion, &acc);
            generator.emit(op::GetCompletionFields {
                type_dst: await_completion_type.operand(),
                value_dst: await_completion_value.operand(),
                completion: await_completion.operand(),
            });
            generator.emit(op::Jump { target: step_block });

            generator.bind_reachable_label(done_block);
        }
    }
}

// https://tc39.es/ecma262/#sec-let-and-const-declarations-runtime-semantics-evaluation
// UsingDeclaration : `using` BindingList `;`
// AwaitUsingDeclaration : CoverAwaitExpressionAndAwaitUsingDeclarationHead BindingList `;`

// https://tc39.es/ecma262/#sec-bindingevaluation
// LexicalBinding : BindingIdentifier Initializer
