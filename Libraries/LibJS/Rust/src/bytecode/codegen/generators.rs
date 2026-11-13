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
use super::stream::Exit;

#[derive(Clone, Copy, PartialEq, Eq)]
enum YieldBehavior {
    Normal,
    AwaitValue,
    IteratorResult,
}

/// The register that a resumed function finds what it was resumed with in.
#[inline(always)]
fn accumulator(_generator: &mut Generator<'_>) -> Operand {
    Operand::register(Register::ACCUMULATOR)
}

/// Emit a property access by a name of the iteration protocol, which is not "length".
fn emit_get_by_known_name(generator: &mut Generator<'_>, dst: Operand, base: Operand, name: Utf16View<'_>) {
    let key = generator.intern_property_key_text(name);
    let cache = generator.next_property_lookup_cache();
    generator.emit(op::GetById {
        dst,
        base,
        property: key,
        cache,
    });
}

/// Generate bytecode that awaits `value`.
pub(super) fn generate_await_of_value(generator: &mut Generator<'_>, value: Operand) -> Operand {
    // Allocate received_completion registers before the await.
    let received_completion = generator.allocate_register();
    let received_completion_type = generator.allocate_register();
    let received_completion_value = generator.allocate_register();
    generate_await_with_completions(
        generator,
        value,
        received_completion,
        received_completion_type,
        received_completion_value,
    )
}

/// Generate bytecode that yields `value`, and goes on with what the generator gets resumed with.
pub(super) fn generate_yield_of_value(
    generator: &mut Generator<'_>,
    value: Operand,
    is_yield_from: bool,
    received_completion: Operand,
    received_completion_type: Operand,
    received_completion_value: Operand,
) -> Operand {
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
        value,
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
    generator.emit_mov(received_completion, acc);
    generator.emit(op::GetCompletionFields {
        type_dst: received_completion_type,
        value_dst: received_completion_value,
        completion: received_completion,
    });

    let normal_block = generator.new_label();
    let throw_cont = generator.new_label();
    let type_is_normal = generator.allocate_register();
    let normal_type = generator.add_constant_number(f64::from(CompletionType::Normal as u8));
    let arith_feedback = generator.next_arith_feedback();
    generator.emit(op::StrictlyEquals {
        arith_feedback,
        dst: type_is_normal,
        lhs: received_completion_type,
        rhs: normal_type,
    });
    generator.emit_jump_if(type_is_normal, normal_block, throw_cont);

    let throw_value_block = generator.new_label();
    let return_value_block = generator.new_label();

    generator.bind_reachable_label(throw_cont);
    let type_is_throw = generator.allocate_register();
    let throw_type = generator.add_constant_number(f64::from(CompletionType::Throw as u8));
    let arith_feedback = generator.next_arith_feedback();
    generator.emit(op::StrictlyEquals {
        arith_feedback,
        dst: type_is_throw,
        lhs: received_completion_type,
        rhs: throw_type,
    });
    generator.emit_jump_if(type_is_throw, throw_value_block, return_value_block);

    generator.bind_reachable_label(throw_value_block);
    generator.emit(op::Throw {
        src: received_completion_value,
    });

    generator.bind_reachable_label(return_value_block);
    generator.emit_exit(Exit::Return, received_completion_value);

    generator.bind_reachable_label(normal_block);
    received_completion_value
}

/// Await `argument` and capture its completion without propagating a rejection.
fn generate_await_completion(
    generator: &mut Generator<'_>,
    argument: Operand,
    received_completion: Operand,
    received_completion_type: Operand,
    received_completion_value: Operand,
) {
    let continuation = generator.new_label();
    generator.emit(op::Await {
        continuation_label: continuation,
        argument,
    });
    generator.bind_reachable_label(continuation);

    let acc = accumulator(generator);
    generator.emit_mov(received_completion, acc);
    generator.emit(op::GetCompletionFields {
        type_dst: received_completion_type,
        value_dst: received_completion_value,
        completion: received_completion,
    });
}

/// Await `argument` using caller-provided completion registers, throwing on rejection.
pub(super) fn generate_await_with_completions(
    generator: &mut Generator<'_>,
    argument: Operand,
    received_completion: Operand,
    received_completion_type: Operand,
    received_completion_value: Operand,
) -> Operand {
    generate_await_completion(
        generator,
        argument,
        received_completion,
        received_completion_type,
        received_completion_value,
    );

    let normal_block = generator.new_label();
    let throw_block = generator.new_label();
    let normal_type = generator.add_constant_number(f64::from(CompletionType::Normal as u8));
    let arith_feedback = generator.next_arith_feedback();
    generator.emit(op::JumpStrictlyEquals {
        arith_feedback,
        lhs: received_completion_type,
        rhs: normal_type,
        true_target: normal_block,
        false_target: throw_block,
    });

    generator.bind_reachable_label(throw_block);
    generator.emit(op::Throw {
        src: received_completion_value,
    });

    generator.bind_reachable_label(normal_block);
    received_completion_value
}

/// Yield* (yield from) delegation.
///
/// Implements the iterator delegation protocol from
/// https://tc39.es/ecma262/#sec-generator-function-definitions-runtime-semantics-evaluation
///
/// The delegating generator forwards next/throw/return to the inner iterator.
fn generate_yield_from(
    generator: &mut Generator<'_>,
    value: Operand,
    received_completion: Operand,
    received_completion_type: Operand,
    received_completion_value: Operand,
) -> Operand {
    let is_async = generator.is_in_async_generator_function();

    // 4. Let iteratorRecord be ? GetIterator(value, generatorKind).
    let iterator = generator.allocate_register();
    let next_method = generator.allocate_register();
    let iterator_done_property = generator.allocate_register();
    let hint = if is_async {
        IteratorHint::Async
    } else {
        IteratorHint::Sync
    } as u8;
    let cache = generator.next_property_lookup_cache();
    generator.emit(op::GetIterator {
        dst_iterator_object: iterator,
        dst_iterator_next: next_method,
        dst_iterator_done: iterator_done_property,
        iterable: value,
        hint,
        // NB: yield* passes the iterator to calls of its methods.
        fast_array_mode_allowed: false,
        cache,
    });

    // 6. Let received be NormalCompletion(undefined).
    let normal_const = generator.add_constant_number(f64::from(CompletionType::Normal as u8));
    generator.emit_mov(received_completion_type, normal_const);
    let undef = generator.add_constant_undefined();
    generator.emit_mov(received_completion_value, undef);

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
        dst: is_normal,
        lhs: received_completion_type,
        rhs: normal_const,
    });
    generator.emit_jump_if(is_normal, type_is_normal_block, is_type_throw_block);

    // =========================================================================
    // a. If received.[[Type]] is normal, then
    // =========================================================================
    generator.bind_reachable_label(type_is_normal_block);

    // i. Let _innerResult_ be ? Call(_next_, _iterator_, « _received_.[[Value]] »).
    let inner_result = generator.allocate_register();
    let call_feedback = generator.next_call_feedback();
    generator.emit(op::Call {
        call_feedback,
        dst: inner_result,
        callee: next_method,
        this_value: iterator,
        argument_count: 1,
        arguments: &[received_completion_value],
    });

    // ii. If _generatorKind_ is ~async~, set _innerResult_ to ? Await(_innerResult_).
    if is_async {
        let awaited = generate_await_with_completions(
            generator,
            inner_result,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
        generator.emit_mov(inner_result, awaited);
    }

    // iii. If _innerResult_ is not an Object, throw a *TypeError* exception.
    generator.emit(op::ThrowIfNotObject { src: inner_result });

    // iv. Let _done_ be ? IteratorComplete(_innerResult_).
    let done = generator.allocate_register();
    emit_get_by_known_name(generator, done, inner_result, utf16!("done").into());

    // v. If _done_ is *true*, then return ? IteratorValue(_innerResult_).
    let type_is_normal_done_block = generator.new_label();
    let type_is_normal_not_done_block = generator.new_label();
    generator.emit_jump_if(done, type_is_normal_done_block, type_is_normal_not_done_block);

    generator.bind_reachable_label(type_is_normal_done_block);
    let return_value = generator.allocate_register();
    emit_get_by_known_name(generator, return_value, inner_result, utf16!("value").into());
    generator.emit(op::Jump { target: loop_end_block });

    // vi. If _generatorKind_ is ~async~, set _received_ to Completion(AsyncGeneratorYield(? IteratorValue(_innerResult_))).
    // vii. Else, set _received_ to Completion(GeneratorYield(_innerResult_)).
    generator.bind_reachable_label(type_is_normal_not_done_block);
    {
        let (yield_value, yield_behavior) = if is_async {
            let current_value = generator.allocate_register();
            emit_get_by_known_name(generator, current_value, inner_result, utf16!("value").into());
            (current_value, YieldBehavior::Normal)
        } else {
            (inner_result, YieldBehavior::IteratorResult)
        };

        generate_yield(
            generator,
            continuation_block,
            yield_value,
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
    let throw_const = generator.add_constant_number(f64::from(CompletionType::Throw as u8));
    let is_throw = generator.allocate_register();
    let arith_feedback = generator.next_arith_feedback();
    generator.emit(op::StrictlyEquals {
        arith_feedback,
        dst: is_throw,
        lhs: received_completion_type,
        rhs: throw_const,
    });
    generator.emit_jump_if(is_throw, type_is_throw_block, type_is_return_block);

    generator.bind_reachable_label(type_is_throw_block);

    // i. Let _throw_ be ? GetMethod(_iterator_, *"throw"*).
    let throw_method = generator.allocate_register();
    let throw_key = generator.intern_property_key_text(utf16!("throw").into());
    let cache = generator.next_property_lookup_cache();
    generator.emit(op::GetMethod {
        dst: throw_method,
        object: iterator,
        property: throw_key,
        cache,
    });

    // ii. If _throw_ is not *undefined*, then
    let throw_method_defined_block = generator.new_label();
    let throw_method_undefined_block = generator.new_label();
    generator.emit(op::JumpUndefined {
        condition: throw_method,
        true_target: throw_method_undefined_block,
        false_target: throw_method_defined_block,
    });

    generator.bind_reachable_label(throw_method_defined_block);

    // 1. Let _innerResult_ be ? Call(_throw_, _iterator_, « _received_.[[Value]] »).
    let call_feedback = generator.next_call_feedback();
    generator.emit(op::Call {
        call_feedback,
        dst: inner_result,
        callee: throw_method,
        this_value: iterator,
        argument_count: 1,
        arguments: &[received_completion_value],
    });

    // 2. If _generatorKind_ is ~async~, set _innerResult_ to ? Await(_innerResult_).
    if is_async {
        let awaited = generate_await_with_completions(
            generator,
            inner_result,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
        generator.emit_mov(inner_result, awaited);
    }

    // 4. If _innerResult_ is not an Object, throw a *TypeError* exception.
    generator.emit(op::ThrowIfNotObject { src: inner_result });

    // 5. Let _done_ be ? IteratorComplete(_innerResult_).
    emit_get_by_known_name(generator, done, inner_result, utf16!("done").into());

    // 6. If _done_ is *true*, return ? IteratorValue(_innerResult_).
    let type_is_throw_done_block = generator.new_label();
    let type_is_throw_not_done_block = generator.new_label();
    generator.emit_jump_if(done, type_is_throw_done_block, type_is_throw_not_done_block);

    generator.bind_reachable_label(type_is_throw_done_block);
    emit_get_by_known_name(generator, return_value, inner_result, utf16!("value").into());
    generator.emit(op::Jump { target: loop_end_block });

    // 7. If _generatorKind_ is ~async~, set _received_ to Completion(AsyncGeneratorYield(? IteratorValue(_innerResult_))).
    // 8. Else, set _received_ to Completion(GeneratorYield(_innerResult_)).
    generator.bind_reachable_label(type_is_throw_not_done_block);
    {
        let (yield_value, yield_behavior) = if is_async {
            let current_value = generator.allocate_register();
            emit_get_by_known_name(generator, current_value, inner_result, utf16!("value").into());
            (current_value, YieldBehavior::Normal)
        } else {
            (inner_result, YieldBehavior::IteratorResult)
        };
        generate_yield(
            generator,
            continuation_block,
            yield_value,
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
            dst: return_method,
            object: iterator,
            property: return_key,
            cache,
        });

        let call_return_block = generator.new_label();
        let after_close = generator.new_label();
        generator.emit(op::JumpUndefined {
            condition: return_method,
            true_target: after_close,
            false_target: call_return_block,
        });
        generator.bind_reachable_label(call_return_block);

        let close_result = generator.allocate_register();
        let call_feedback = generator.next_call_feedback();
        generator.emit(op::Call {
            call_feedback,
            dst: close_result,
            callee: return_method,
            this_value: iterator,
            argument_count: 0,
            arguments: &[],
        });

        let awaited = generate_await_with_completions(
            generator,
            close_result,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
        generator.emit(op::ThrowIfNotObject { src: awaited });

        generator.emit(op::Jump { target: after_close });
        generator.bind_reachable_label(after_close);
    } else {
        // Sync: IteratorClose with Normal completion.
        let undef = generator.add_constant_undefined();
        let cache = generator.next_property_lookup_cache();
        generator.emit(op::IteratorClose {
            iterator_object: iterator,
            iterator_next: next_method,
            iterator_done: iterator_done_property,
            completion_type: CompletionType::Normal as u8,
            completion_value: undef,
            cache,
        });
    }

    // Throw a TypeError: iterator does not have a throw method.
    let exception = generator.allocate_register();
    let error_string =
        generator.intern_string_text(utf16!("yield* protocol violation: iterator must have a throw method").into());
    generator.emit(op::NewTypeError {
        dst: exception,
        error_string,
    });
    generator.emit(op::Throw { src: exception });

    // =========================================================================
    // c. Else (received.[[Type]] is return)
    // =========================================================================
    generator.bind_reachable_label(type_is_return_block);

    // ii. Let _return_ be ? GetMethod(_iterator_, *"return"*).
    let return_method = generator.allocate_register();
    let return_key = generator.intern_property_key_text(utf16!("return").into());
    let cache = generator.next_property_lookup_cache();
    generator.emit(op::GetMethod {
        dst: return_method,
        object: iterator,
        property: return_key,
        cache,
    });

    // iii. If _return_ is *undefined*, then
    let return_is_undefined_block = generator.new_label();
    let return_is_defined_block = generator.new_label();
    generator.emit(op::JumpUndefined {
        condition: return_method,
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
    generator.emit_exit(Exit::Return, received_completion_value);

    generator.bind_reachable_label(return_is_defined_block);

    // iv. Let _innerReturnResult_ be ? Call(_return_, _iterator_, « _received_.[[Value]] »).
    let inner_return_result = generator.allocate_register();
    let call_feedback = generator.next_call_feedback();
    generator.emit(op::Call {
        call_feedback,
        dst: inner_return_result,
        callee: return_method,
        this_value: iterator,
        argument_count: 1,
        arguments: &[received_completion_value],
    });

    // v. If _generatorKind_ is ~async~, set _innerReturnResult_ to ? Await(_innerReturnResult_).
    if is_async {
        let awaited = generate_await_with_completions(
            generator,
            inner_return_result,
            received_completion,
            received_completion_type,
            received_completion_value,
        );
        generator.emit_mov(inner_return_result, awaited);
    }

    // vi. If _innerReturnResult_ is not an Object, throw a *TypeError* exception.
    generator.emit(op::ThrowIfNotObject {
        src: inner_return_result,
    });

    // vii. Let _done_ be ? IteratorComplete(_innerReturnResult_).
    emit_get_by_known_name(generator, done, inner_return_result, utf16!("done").into());

    // viii. If _done_ is *true*, then
    let type_is_return_done_block = generator.new_label();
    let type_is_return_not_done_block = generator.new_label();
    generator.emit_jump_if(done, type_is_return_done_block, type_is_return_not_done_block);

    generator.bind_reachable_label(type_is_return_done_block);
    let inner_return_result_value = generator.allocate_register();
    emit_get_by_known_name(
        generator,
        inner_return_result_value,
        inner_return_result,
        utf16!("value").into(),
    );
    generator.emit_exit(Exit::Return, inner_return_result_value);

    // ix. If _generatorKind_ is ~async~, set _received_ to Completion(AsyncGeneratorYield(? IteratorValue(_innerReturnResult_))).
    // x. Else, set _received_ to Completion(GeneratorYield(_innerReturnResult_)).
    generator.bind_reachable_label(type_is_return_not_done_block);
    let (received, yield_behavior) = if is_async {
        let current_value = generator.allocate_register();
        emit_get_by_known_name(generator, current_value, inner_return_result, utf16!("value").into());
        (current_value, YieldBehavior::Normal)
    } else {
        (inner_return_result, YieldBehavior::IteratorResult)
    };
    generate_yield(
        generator,
        continuation_block,
        received,
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
    generator.emit_mov(received_completion, acc);
    generator.emit(op::GetCompletionFields {
        type_dst: received_completion_type,
        value_dst: received_completion_value,
        completion: received_completion,
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
    argument: Operand,
    received_completion: Operand,
    received_completion_type: Operand,
    received_completion_value: Operand,
    yield_behavior: YieldBehavior,
) {
    if !generator.is_in_async_generator_function() {
        if yield_behavior == YieldBehavior::IteratorResult {
            generator.emit(op::YieldIteratorResult {
                continuation_label,
                value: argument,
            });
        } else {
            generator.emit(op::Yield {
                continuation_label: Some(continuation_label),
                value: argument,
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
        argument
    };

    // Yield, then UnwrapYieldResumption.
    let unwrap_block = generator.new_label();
    generator.emit(op::Yield {
        continuation_label: Some(unwrap_block),
        value: argument,
    });
    generator.bind_reachable_label(unwrap_block);

    let acc = accumulator(generator);
    generator.emit_mov(received_completion, acc);
    generator.emit(op::GetCompletionFields {
        type_dst: received_completion_type,
        value_dst: received_completion_value,
        completion: received_completion,
    });

    // If resumptionValue.[[Type]] is not return, jump to continuation.
    let return_block = generator.new_label();
    let is_not_return = generator.allocate_register();
    let return_type = generator.add_constant_number(f64::from(CompletionType::Return as u8));
    let arith_feedback = generator.next_arith_feedback();
    generator.emit(op::StrictlyInequals {
        arith_feedback,
        dst: is_not_return,
        lhs: received_completion_type,
        rhs: return_type,
    });
    generator.emit_jump_if(is_not_return, continuation_label, return_block);

    // Return path: Await(resumptionValue.[[Value]]).
    generator.bind_reachable_label(return_block);
    // NB: A rejection must reach the yield continuation so yield* can forward it to the delegated iterator.
    generate_await_completion(
        generator,
        received_completion_value,
        received_completion,
        received_completion_type,
        received_completion_value,
    );

    // If awaited.[[Type]] is throw, jump to continuation.
    let awaited_normal_block = generator.new_label();
    let is_throw = generator.allocate_register();
    let throw_type = generator.add_constant_number(f64::from(CompletionType::Throw as u8));
    let arith_feedback = generator.next_arith_feedback();
    generator.emit(op::StrictlyEquals {
        arith_feedback,
        dst: is_throw,
        lhs: received_completion_type,
        rhs: throw_type,
    });
    generator.emit_jump_if(is_throw, continuation_label, awaited_normal_block);

    // awaited.[[Type]] is normal: set type to Return and jump to continuation.
    generator.bind_reachable_label(awaited_normal_block);
    generator.emit(op::SetCompletionType {
        completion: received_completion,
        completion_type: CompletionType::Return as u8,
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
    stack: Operand,
    completion_type: Operand,
    completion_value: Operand,
) {
    match kind {
        DisposalKind::Sync => {
            generator.emit(op::DisposeResources {
                stack,
                completion_type,
                completion_value,
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
            let normal_type = generator.add_constant_number(f64::from(CompletionType::Normal as u8));
            let undefined = generator.add_constant_undefined();
            generator.emit_mov(await_completion_type, normal_type);
            generator.emit_mov(await_completion_value, undefined);

            let step_block = generator.new_label();
            let await_block = generator.new_label();
            let continuation_block = generator.new_label();
            let done_block = generator.new_label();
            generator.emit(op::Jump { target: step_block });

            generator.bind_reachable_label(step_block);
            generator.emit(op::AsyncDisposeResourcesStep {
                done,
                awaited_value,
                stack,
                completion_type,
                completion_value,
                await_completion_type,
                await_completion_value,
            });
            generator.emit_jump_if(done, done_block, await_block);

            generator.bind_reachable_label(await_block);
            generator.emit(op::Await {
                continuation_label: continuation_block,
                argument: awaited_value,
            });

            generator.bind_reachable_label(continuation_block);
            let acc = Operand::register(Register::ACCUMULATOR);
            generator.emit_mov(await_completion, acc);
            generator.emit(op::GetCompletionFields {
                type_dst: await_completion_type,
                value_dst: await_completion_value,
                completion: await_completion,
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
