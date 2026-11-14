/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Yield, await and yield*, and disposing of resources, which can take awaiting.

use libjs_abi::CompletionType;
use libjs_abi::IteratorHint;

use crate::ast::BinaryOp;
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

/// Where code goes on after a yield: `resumed` with the completion still in the accumulator, and `received` once
/// the completion registers hold it and its fields.
#[derive(Clone, Copy)]
struct YieldContinuation {
    resumed: Label,
    received: Label,
}

impl YieldContinuation {
    fn new(generator: &mut Generator<'_>) -> Self {
        Self {
            resumed: generator.new_label(),
            received: generator.new_label(),
        }
    }

    fn bind(self, generator: &mut Generator<'_>, received: CompletionRegisters) {
        // NB: Only a sync generator resumes at `resumed`. An async one reads the completion while unwrapping it.
        if !generator.is_in_async_generator_function() {
            generator.bind_reachable_label(self.resumed);
            received.receive(generator);
        }
        generator.bind_reachable_label(self.received);
    }
}

/// The registers that hold a completion that a suspended function was resumed with, and its fields.
#[derive(Clone, Copy)]
pub(super) struct CompletionRegisters {
    pub(super) completion: Operand,
    pub(super) completion_type: Operand,
    pub(super) value: Operand,
}

impl CompletionRegisters {
    /// The caller's held-register region owns these registers across suspension and resumption.
    pub(super) fn allocate(generator: &mut Generator<'_>) -> Self {
        Self {
            completion: generator.held_register(),
            completion_type: generator.held_register(),
            value: generator.held_register(),
        }
    }

    /// Jumps to `true_target` if the completion is of `completion_type`, and to `false_target` otherwise.
    fn jump_if_type(
        self,
        generator: &mut Generator<'_>,
        completion_type: CompletionType,
        true_target: Label,
        false_target: Label,
    ) {
        let completion_type = generator.add_constant_number(f64::from(completion_type as u8));
        generator.emit_comparison_jump(
            BinaryOp::StrictlyEquals,
            self.completion_type,
            completion_type,
            true_target,
            false_target,
        );
    }

    /// Takes the completion that the function was resumed with from the accumulator.
    fn receive(self, generator: &mut Generator<'_>) {
        generator.emit_mov(self.completion, Operand::register(Register::ACCUMULATOR));
        self.read_fields(generator);
    }

    fn read_fields(self, generator: &mut Generator<'_>) {
        generator.put(op::GetCompletionFields {
            type_dst: self.completion_type,
            value_dst: self.value,
            completion: self.completion,
        });
    }
}

/// Emit a property access by a name of the iteration protocol, which is not "length".
fn emit_get_by_known_name(generator: &mut Generator<'_>, dst: Operand, base: Operand, name: Utf16View<'_>) {
    let key = generator.intern_property_key_text(name);
    let cache = generator.next_property_lookup_cache();
    generator.put(op::GetById {
        dst,
        base,
        property: key,
        cache,
    });
}

/// Generate bytecode that awaits `value`.
pub(super) fn generate_await_of_value(generator: &mut Generator<'_>, value: Operand) -> Operand {
    let received = CompletionRegisters::allocate(generator);
    generate_await_with_completions(generator, value, received)
}

/// Generate bytecode that yields `value`, and goes on with what the generator gets resumed with.
pub(super) fn generate_yield_of_value(
    generator: &mut Generator<'_>,
    value: Operand,
    is_yield_from: bool,
    received: CompletionRegisters,
) -> Operand {
    if is_yield_from {
        return generate_yield_from(generator, value, received);
    }

    let continuation = YieldContinuation::new(generator);
    generate_yield(
        generator,
        continuation,
        value,
        received,
        if generator.is_in_async_generator_function() {
            YieldBehavior::AwaitValue
        } else {
            YieldBehavior::Normal
        },
    );

    continuation.bind(generator, received);

    let normal_block = generator.new_label();
    let throw_cont = generator.new_label();
    received.jump_if_type(generator, CompletionType::Normal, normal_block, throw_cont);

    let throw_value_block = generator.new_label();
    let return_value_block = generator.new_label();

    generator.bind_reachable_label(throw_cont);
    received.jump_if_type(generator, CompletionType::Throw, throw_value_block, return_value_block);

    generator.bind_reachable_label(throw_value_block);
    generator.put(op::Throw { src: received.value });

    generator.bind_reachable_label(return_value_block);
    generator.emit_exit(Exit::Return, received.value);

    generator.bind_reachable_label(normal_block);
    received.value
}

/// Await `argument` and capture its completion without propagating a rejection.
fn generate_await_completion(generator: &mut Generator<'_>, argument: Operand, received: CompletionRegisters) {
    let continuation = generator.new_label();
    generator.put(op::Await {
        continuation_label: continuation,
        argument,
    });
    generator.bind_reachable_label(continuation);

    received.receive(generator);
}

/// Await `argument` using caller-provided completion registers, throwing on rejection.
pub(super) fn generate_await_with_completions(
    generator: &mut Generator<'_>,
    argument: Operand,
    received: CompletionRegisters,
) -> Operand {
    generate_await_completion(generator, argument, received);

    let normal_block = generator.new_label();
    let throw_block = generator.new_label();
    received.jump_if_type(generator, CompletionType::Normal, normal_block, throw_block);

    generator.bind_reachable_label(throw_block);
    generator.put(op::Throw { src: received.value });

    generator.bind_reachable_label(normal_block);
    received.value
}

/// Yield* (yield from) delegation.
///
/// Implements the iterator delegation protocol from
/// https://tc39.es/ecma262/#sec-generator-function-definitions-runtime-semantics-evaluation
///
/// The delegating generator forwards next/throw/return to the inner iterator.
fn generate_yield_from(generator: &mut Generator<'_>, value: Operand, received: CompletionRegisters) -> Operand {
    let is_async = generator.is_in_async_generator_function();

    // 4. Let iteratorRecord be ? GetIterator(value, generatorKind).
    let iterator = generator.held_register();
    let next_method = generator.held_register();
    let inner_result = generator.held_register();
    let done = generator.held_register();
    let return_value = generator.held_register();
    let hint = if is_async {
        IteratorHint::Async
    } else {
        IteratorHint::Sync
    } as u8;
    let cache = generator.next_property_lookup_cache();
    generator.put(op::GetIterator {
        dst_iterator_object: iterator,
        dst_iterator_next: next_method,
        // NB: The iterator of yield* is never done before its first step, and `done` gets written before it is read.
        dst_iterator_done: done,
        iterable: value,
        hint,
        // NB: yield* passes the iterator to calls of its methods.
        fast_array_mode_allowed: false,
        cache,
    });

    // 6. Let received be NormalCompletion(undefined).
    let normal_const = generator.add_constant_number(f64::from(CompletionType::Normal as u8));
    generator.emit_mov(received.completion_type, normal_const);
    let undef = generator.add_constant_undefined();
    generator.emit_mov(received.value, undef);

    // 7. Repeat,
    let loop_block = generator.new_label();
    let continuation = YieldContinuation::new(generator);
    let loop_end_block = generator.new_label();
    generator.put(op::Jump { target: loop_block });
    generator.bind_reachable_label(loop_block);

    // Branch on received.[[Type]].
    let type_is_normal_block = generator.new_label();
    let is_type_throw_block = generator.new_label();
    received.jump_if_type(
        generator,
        CompletionType::Normal,
        type_is_normal_block,
        is_type_throw_block,
    );

    // =========================================================================
    // a. If received.[[Type]] is normal, then
    // =========================================================================
    generator.bind_reachable_label(type_is_normal_block);
    // NB: Each branch ends by yielding, throwing or leaving the loop, so the registers that it holds do not outlive it.
    let branch_registers = generator.hold_registers();

    // i. Let _innerResult_ be ? Call(_next_, _iterator_, « _received_.[[Value]] »).
    let call_feedback = generator.next_call_feedback();
    generator.put(op::Call {
        call_feedback,
        dst: inner_result,
        callee: next_method,
        this_value: iterator,
        argument_count: 1,
        arguments: &[received.value],
    });

    // ii. If _generatorKind_ is ~async~, set _innerResult_ to ? Await(_innerResult_).
    if is_async {
        let awaited = generate_await_with_completions(generator, inner_result, received);
        generator.emit_mov(inner_result, awaited);
    }

    // iii. If _innerResult_ is not an Object, throw a *TypeError* exception.
    generator.put(op::ThrowIfNotObject { src: inner_result });

    // iv. Let _done_ be ? IteratorComplete(_innerResult_).
    emit_get_by_known_name(generator, done, inner_result, utf16!("done").into());

    // v. If _done_ is *true*, then return ? IteratorValue(_innerResult_).
    let type_is_normal_done_block = generator.new_label();
    let type_is_normal_not_done_block = generator.new_label();
    generator.emit_jump_if(done, type_is_normal_done_block, type_is_normal_not_done_block);

    generator.bind_reachable_label(type_is_normal_done_block);
    emit_get_by_known_name(generator, return_value, inner_result, utf16!("value").into());
    generator.put(op::Jump { target: loop_end_block });

    // vi. If _generatorKind_ is ~async~, set _received_ to Completion(AsyncGeneratorYield(? IteratorValue(_innerResult_))).
    // vii. Else, set _received_ to Completion(GeneratorYield(_innerResult_)).
    generator.bind_reachable_label(type_is_normal_not_done_block);
    {
        let (yield_value, yield_behavior) = if is_async {
            let current_value = generator.held_register();
            emit_get_by_known_name(generator, current_value, inner_result, utf16!("value").into());
            (current_value, YieldBehavior::Normal)
        } else {
            (inner_result, YieldBehavior::IteratorResult)
        };

        generate_yield(generator, continuation, yield_value, received, yield_behavior);
    }
    generator.release_held_registers(branch_registers);

    // =========================================================================
    // b. Else if received.[[Type]] is throw, then
    // =========================================================================
    generator.bind_reachable_label(is_type_throw_block);
    let type_is_throw_block = generator.new_label();
    let type_is_return_block = generator.new_label();
    received.jump_if_type(
        generator,
        CompletionType::Throw,
        type_is_throw_block,
        type_is_return_block,
    );

    generator.bind_reachable_label(type_is_throw_block);
    let branch_registers = generator.hold_registers();

    // i. Let _throw_ be ? GetMethod(_iterator_, *"throw"*).
    let throw_method = generator.held_register();
    let throw_key = generator.intern_property_key_text(utf16!("throw").into());
    let cache = generator.next_property_lookup_cache();
    generator.put(op::GetMethod {
        dst: throw_method,
        object: iterator,
        property: throw_key,
        cache,
    });

    // ii. If _throw_ is not *undefined*, then
    let throw_method_defined_block = generator.new_label();
    let throw_method_undefined_block = generator.new_label();
    generator.put(op::JumpUndefined {
        condition: throw_method,
        true_target: throw_method_undefined_block,
        false_target: throw_method_defined_block,
    });

    generator.bind_reachable_label(throw_method_defined_block);

    // 1. Let _innerResult_ be ? Call(_throw_, _iterator_, « _received_.[[Value]] »).
    let call_feedback = generator.next_call_feedback();
    generator.put(op::Call {
        call_feedback,
        dst: inner_result,
        callee: throw_method,
        this_value: iterator,
        argument_count: 1,
        arguments: &[received.value],
    });

    // 2. If _generatorKind_ is ~async~, set _innerResult_ to ? Await(_innerResult_).
    if is_async {
        let awaited = generate_await_with_completions(generator, inner_result, received);
        generator.emit_mov(inner_result, awaited);
    }

    // 4. If _innerResult_ is not an Object, throw a *TypeError* exception.
    generator.put(op::ThrowIfNotObject { src: inner_result });

    // 5. Let _done_ be ? IteratorComplete(_innerResult_).
    emit_get_by_known_name(generator, done, inner_result, utf16!("done").into());

    // 6. If _done_ is *true*, return ? IteratorValue(_innerResult_).
    let type_is_throw_done_block = generator.new_label();
    let type_is_throw_not_done_block = generator.new_label();
    generator.emit_jump_if(done, type_is_throw_done_block, type_is_throw_not_done_block);

    generator.bind_reachable_label(type_is_throw_done_block);
    emit_get_by_known_name(generator, return_value, inner_result, utf16!("value").into());
    generator.put(op::Jump { target: loop_end_block });

    // 7. If _generatorKind_ is ~async~, set _received_ to Completion(AsyncGeneratorYield(? IteratorValue(_innerResult_))).
    // 8. Else, set _received_ to Completion(GeneratorYield(_innerResult_)).
    generator.bind_reachable_label(type_is_throw_not_done_block);
    {
        let (yield_value, yield_behavior) = if is_async {
            let current_value = generator.held_register();
            emit_get_by_known_name(generator, current_value, inner_result, utf16!("value").into());
            (current_value, YieldBehavior::Normal)
        } else {
            (inner_result, YieldBehavior::IteratorResult)
        };
        generate_yield(generator, continuation, yield_value, received, yield_behavior);
    }

    // throw is undefined: close iterator, throw TypeError.
    generator.bind_reachable_label(throw_method_undefined_block);

    if is_async {
        // AsyncIteratorClose: get return method, call it, await, check object.
        let return_method = generator.held_register();
        let return_key = generator.intern_property_key_text(utf16!("return").into());
        let cache = generator.next_property_lookup_cache();
        generator.put(op::GetMethod {
            dst: return_method,
            object: iterator,
            property: return_key,
            cache,
        });

        let call_return_block = generator.new_label();
        let after_close = generator.new_label();
        generator.put(op::JumpUndefined {
            condition: return_method,
            true_target: after_close,
            false_target: call_return_block,
        });
        generator.bind_reachable_label(call_return_block);

        let close_result = generator.held_register();
        let call_feedback = generator.next_call_feedback();
        generator.put(op::Call {
            call_feedback,
            dst: close_result,
            callee: return_method,
            this_value: iterator,
            argument_count: 0,
            arguments: &[],
        });

        let awaited = generate_await_with_completions(generator, close_result, received);
        generator.put(op::ThrowIfNotObject { src: awaited });

        generator.put(op::Jump { target: after_close });
        generator.bind_reachable_label(after_close);
    } else {
        // Sync: IteratorClose with Normal completion.
        let undef = generator.add_constant_undefined();
        let not_done = generator.add_constant_boolean(false);
        let cache = generator.next_property_lookup_cache();
        generator.put(op::IteratorClose {
            iterator_object: iterator,
            iterator_next: next_method,
            iterator_done: not_done,
            completion_type: CompletionType::Normal as u8,
            completion_value: undef,
            cache,
        });
    }

    // Throw a TypeError: iterator does not have a throw method.
    let exception = generator.held_register();
    let error_string =
        generator.intern_string_text(utf16!("yield* protocol violation: iterator must have a throw method").into());
    generator.put(op::NewTypeError {
        dst: exception,
        error_string,
    });
    generator.put(op::Throw { src: exception });
    generator.release_held_registers(branch_registers);

    // =========================================================================
    // c. Else (received.[[Type]] is return)
    // =========================================================================
    generator.bind_reachable_label(type_is_return_block);
    let branch_registers = generator.hold_registers();

    // ii. Let _return_ be ? GetMethod(_iterator_, *"return"*).
    let return_method = generator.held_register();
    let return_key = generator.intern_property_key_text(utf16!("return").into());
    let cache = generator.next_property_lookup_cache();
    generator.put(op::GetMethod {
        dst: return_method,
        object: iterator,
        property: return_key,
        cache,
    });

    // iii. If _return_ is *undefined*, then
    let return_is_undefined_block = generator.new_label();
    let return_is_defined_block = generator.new_label();
    generator.put(op::JumpUndefined {
        condition: return_method,
        true_target: return_is_undefined_block,
        false_target: return_is_defined_block,
    });

    generator.bind_reachable_label(return_is_undefined_block);
    // 1. If _generatorKind_ is ~async~, set _received_.[[Value]] to ? Await(_received_.[[Value]]).
    if is_async {
        generate_await_with_completions(generator, received.value, received);
    }
    // 2. Return _received_.
    generator.emit_exit(Exit::Return, received.value);

    generator.bind_reachable_label(return_is_defined_block);

    // iv. Let _innerReturnResult_ be ? Call(_return_, _iterator_, « _received_.[[Value]] »).
    let inner_return_result = generator.held_register();
    let call_feedback = generator.next_call_feedback();
    generator.put(op::Call {
        call_feedback,
        dst: inner_return_result,
        callee: return_method,
        this_value: iterator,
        argument_count: 1,
        arguments: &[received.value],
    });

    // v. If _generatorKind_ is ~async~, set _innerReturnResult_ to ? Await(_innerReturnResult_).
    if is_async {
        let awaited = generate_await_with_completions(generator, inner_return_result, received);
        generator.emit_mov(inner_return_result, awaited);
    }

    // vi. If _innerReturnResult_ is not an Object, throw a *TypeError* exception.
    generator.put(op::ThrowIfNotObject {
        src: inner_return_result,
    });

    // vii. Let _done_ be ? IteratorComplete(_innerReturnResult_).
    emit_get_by_known_name(generator, done, inner_return_result, utf16!("done").into());

    // viii. If _done_ is *true*, then
    let type_is_return_done_block = generator.new_label();
    let type_is_return_not_done_block = generator.new_label();
    generator.emit_jump_if(done, type_is_return_done_block, type_is_return_not_done_block);

    generator.bind_reachable_label(type_is_return_done_block);
    let inner_return_result_value = generator.held_register();
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
    let (yielded, yield_behavior) = if is_async {
        let current_value = generator.held_register();
        emit_get_by_known_name(generator, current_value, inner_return_result, utf16!("value").into());
        (current_value, YieldBehavior::Normal)
    } else {
        (inner_return_result, YieldBehavior::IteratorResult)
    };
    generate_yield(generator, continuation, yielded, received, yield_behavior);
    generator.release_held_registers(branch_registers);

    // =========================================================================
    // Continuation block: resume after any yield, extract completion, loop back.
    // =========================================================================
    continuation.bind(generator, received);
    generator.put(op::Jump { target: loop_block });

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
/// Goes on at `continuation`, and for an async generator with the completion fields read.
fn generate_yield(
    generator: &mut Generator<'_>,
    continuation: YieldContinuation,
    argument: Operand,
    received: CompletionRegisters,
    yield_behavior: YieldBehavior,
) {
    if !generator.is_in_async_generator_function() {
        if yield_behavior == YieldBehavior::IteratorResult {
            generator.put(op::YieldIteratorResult {
                continuation_label: continuation.resumed,
                value: argument,
            });
        } else {
            generator.put(op::Yield {
                continuation_label: Some(continuation.resumed),
                value: argument,
            });
        }
        return;
    }

    let argument = if yield_behavior == YieldBehavior::AwaitValue {
        generate_await_with_completions(generator, argument, received)
    } else {
        argument
    };

    // Yield, then UnwrapYieldResumption.
    let unwrap_block = generator.new_label();
    generator.put(op::Yield {
        continuation_label: Some(unwrap_block),
        value: argument,
    });
    generator.bind_reachable_label(unwrap_block);

    received.receive(generator);

    // If resumptionValue.[[Type]] is not return, jump to continuation.
    let return_block = generator.new_label();
    received.jump_if_type(generator, CompletionType::Return, return_block, continuation.received);

    // Return path: Await(resumptionValue.[[Value]]).
    generator.bind_reachable_label(return_block);
    // NB: A rejection must reach the yield continuation so yield* can forward it to the delegated iterator.
    generate_await_completion(generator, received.value, received);

    // If awaited.[[Type]] is throw, jump to continuation.
    let awaited_normal_block = generator.new_label();
    received.jump_if_type(
        generator,
        CompletionType::Throw,
        continuation.received,
        awaited_normal_block,
    );

    // awaited.[[Type]] is normal: set type to Return and jump to continuation.
    generator.bind_reachable_label(awaited_normal_block);
    generator.put(op::SetCompletionType {
        completion: received.completion,
        completion_type: CompletionType::Return as u8,
    });
    // NB: SetCompletionType changes the completion itself, so its fields have to be read again.
    received.read_fields(generator);
    generator.jump(continuation.received);
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
            generator.put(op::DisposeResources {
                stack,
                completion_type,
                completion_value,
            });
        }
        DisposalKind::Async => {
            // NB: DisposeResources awaits between some of its steps here, so the bytecode performs it one step at a
            //     time, with an Await after each step that asks for one.
            let awaited = CompletionRegisters::allocate(generator);
            let done = generator.held_register();
            let awaited_value = generator.held_register();
            let normal_type = generator.add_constant_number(f64::from(CompletionType::Normal as u8));
            let undefined = generator.add_constant_undefined();
            generator.emit_mov(awaited.completion_type, normal_type);
            generator.emit_mov(awaited.value, undefined);

            let step_block = generator.new_label();
            let await_block = generator.new_label();
            let continuation_block = generator.new_label();
            let done_block = generator.new_label();
            generator.put(op::Jump { target: step_block });

            generator.bind_reachable_label(step_block);
            generator.put(op::AsyncDisposeResourcesStep {
                done,
                awaited_value,
                stack,
                completion_type,
                completion_value,
                await_completion_type: awaited.completion_type,
                await_completion_value: awaited.value,
            });
            generator.emit_jump_if(done, done_block, await_block);

            generator.bind_reachable_label(await_block);
            generator.put(op::Await {
                continuation_label: continuation_block,
                argument: awaited_value,
            });

            generator.bind_reachable_label(continuation_block);
            awaited.receive(generator);
            generator.put(op::Jump { target: step_block });

            generator.bind_reachable_label(done_block);
        }
    }
}

// https://tc39.es/ecma262/#sec-let-and-const-declarations-runtime-semantics-evaluation
// UsingDeclaration : `using` BindingList `;`
// AwaitUsingDeclaration : CoverAwaitExpressionAndAwaitUsingDeclarationHead BindingList `;`

// https://tc39.es/ecma262/#sec-bindingevaluation
// LexicalBinding : BindingIdentifier Initializer
