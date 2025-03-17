/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <LibJS/Bytecode/Generator.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/GeneratorObject.h>
#include <LibJS/Runtime/GeneratorPrototype.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>

namespace JS {

GC_DEFINE_ALLOCATOR(GeneratorObject);

ThrowCompletionOr<GC::Ref<GeneratorObject>> GeneratorObject::create(Realm& realm, Value initial_value, ECMAScriptFunctionObject* generating_function, NonnullOwnPtr<ExecutionContext> execution_context)
{
    auto& vm = realm.vm();
    // This is "g1.prototype" in figure-2 (https://tc39.es/ecma262/img/figure-2.png)
    Value generating_function_prototype;
    if (generating_function->kind() == FunctionKind::Async) {
        // We implement async functions by transforming them to generator function in the bytecode
        // interpreter. However an async function does not have a prototype and should not be
        // changed thus we hardcode the prototype.
        generating_function_prototype = realm.intrinsics().generator_prototype();
    } else {
        generating_function_prototype = TRY(generating_function->get(vm.names.prototype));
    }
    auto generating_function_prototype_object = TRY(generating_function_prototype.to_object(vm));
    auto object = realm.create<GeneratorObject>(realm, generating_function_prototype_object, move(execution_context));
    object->m_generating_function = generating_function;
    object->m_previous_value = initial_value;
    return object;
}

GeneratorObject::GeneratorObject(Realm&, Object& prototype, NonnullOwnPtr<ExecutionContext> context, Optional<StringView> generator_brand)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_execution_context(move(context))
    , m_generator_brand(move(generator_brand))
{
}

void GeneratorObject::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_generating_function);
    visitor.visit(m_previous_value);
    m_execution_context->visit_edges(visitor);
}

// 27.5.3.2 GeneratorValidate ( generator, generatorBrand ), https://tc39.es/ecma262/#sec-generatorvalidate
ThrowCompletionOr<GeneratorObject::GeneratorState> GeneratorObject::validate(VM& vm, Optional<StringView> const& generator_brand)
{
    // 1. Perform ? RequireInternalSlot(generator, [[GeneratorState]]).
    // 2. Perform ? RequireInternalSlot(generator, [[GeneratorBrand]]).
    // NOTE: Already done by the caller of resume or resume_abrupt, as they wouldn't have a GeneratorObject otherwise.

    // 3. If generator.[[GeneratorBrand]] is not the same value as generatorBrand, throw a TypeError exception.
    if (m_generator_brand != generator_brand)
        return vm.throw_completion<TypeError>(ErrorType::GeneratorBrandMismatch, m_generator_brand.value_or("<empty>"sv), generator_brand.value_or("<empty>"sv));

    // 4. Assert: generator also has a [[GeneratorContext]] internal slot.
    // NOTE: Done by already being a GeneratorObject.

    // 5. Let state be generator.[[GeneratorState]].
    auto state = m_generator_state;

    // 6. If state is executing, throw a TypeError exception.
    if (state == GeneratorState::Executing)
        return vm.throw_completion<TypeError>(ErrorType::GeneratorAlreadyExecuting);

    // 7. Return state.
    return state;
}

ThrowCompletionOr<Value> GeneratorObject::execute(VM& vm, Completion const& completion)
{
    // Loosely based on step 4 of https://tc39.es/ecma262/#sec-generatorstart mixed with https://tc39.es/ecma262/#sec-generatoryield at the end.

    VERIFY(completion.value().has_value());

    auto generated_value = [&vm](Value value) -> Value {
        if (value.is_object())
            return value.as_object().get_without_side_effects(vm.names.result);
        return value.is_empty() ? js_undefined() : value;
    };

    auto generated_continuation = [&](Value value) -> Optional<size_t> {
        if (value.is_object()) {
            auto number_value = value.as_object().get_without_side_effects(vm.names.continuation);
            if (number_value.is_null())
                return {};
            return static_cast<u64>(number_value.as_double());
        }
        return {};
    };

    auto& realm = *vm.current_realm();
    auto completion_object = Object::create(realm, nullptr);
    completion_object->define_direct_property(vm.names.type, Value(to_underlying(completion.type())), default_attributes);
    completion_object->define_direct_property(vm.names.value, completion.value().value(), default_attributes);

    auto& bytecode_interpreter = vm.bytecode_interpreter();

    auto const next_block = generated_continuation(m_previous_value);

    // We should never enter `execute` again after the generator is complete.
    VERIFY(next_block.has_value());

    auto next_result = bytecode_interpreter.run_executable(*m_generating_function->bytecode_executable(), next_block, completion_object);

    vm.pop_execution_context();

    auto result_value = move(next_result.value);
    if (result_value.is_throw_completion()) {
        // Uncaught exceptions disable the generator.
        m_generator_state = GeneratorState::Completed;
        return result_value;
    }
    m_previous_value = result_value.release_value();
    bool done = !generated_continuation(m_previous_value).has_value();

    m_generator_state = done ? GeneratorState::Completed : GeneratorState::SuspendedYield;
    return create_iterator_result_object(vm, generated_value(m_previous_value), done);
}

// 27.5.3.3 GeneratorResume ( generator, value, generatorBrand ), https://tc39.es/ecma262/#sec-generatorresume
ThrowCompletionOr<Value> GeneratorObject::resume(VM& vm, Value value, Optional<StringView> const& generator_brand)
{
    // 1. Let state be ? GeneratorValidate(generator, generatorBrand).
    auto state = TRY(validate(vm, generator_brand));

    // 2. If state is completed, return CreateIterResultObject(undefined, true).
    if (state == GeneratorState::Completed)
        return create_iterator_result_object(vm, js_undefined(), true);

    // 3. Assert: state is either suspendedStart or suspendedYield.
    VERIFY(state == GeneratorState::SuspendedStart || state == GeneratorState::SuspendedYield);

    // 4. Let genContext be generator.[[GeneratorContext]].
    auto& generator_context = m_execution_context;

    // 5. Let methodContext be the running execution context.
    auto const& method_context = vm.running_execution_context();

    // 6. Suspend methodContext.
    // 8. Push genContext onto the execution context stack; genContext is now the running execution context.
    // NOTE: This is done out of order as to not permanently disable the generator if push_execution_context throws,
    //       as `resume` will immediately throw when [[GeneratorState]] is "executing", never allowing the state to change.
    TRY(vm.push_execution_context(*generator_context, {}));

    // 7. Set generator.[[GeneratorState]] to executing.
    m_generator_state = GeneratorState::Executing;

    // 9. Resume the suspended evaluation of genContext using NormalCompletion(value) as the result of the operation that suspended it. Let result be the value returned by the resumed computation.
    auto result = execute(vm, normal_completion(value));

    // 10. Assert: When we return here, genContext has already been removed from the execution context stack and methodContext is the currently running execution context.
    VERIFY(&vm.running_execution_context() == &method_context);

    // 11. Return ? result.
    return result;
}

// 27.5.3.4 GeneratorResumeAbrupt ( generator, abruptCompletion, generatorBrand ), https://tc39.es/ecma262/#sec-generatorresumeabrupt
ThrowCompletionOr<Value> GeneratorObject::resume_abrupt(JS::VM& vm, JS::Completion abrupt_completion, Optional<StringView> const& generator_brand)
{
    // Not part of the spec, but the spec assumes abruptCompletion.[[Value]] is not empty.
    VERIFY(abrupt_completion.value().has_value());

    // 1. Let state be ? GeneratorValidate(generator, generatorBrand).
    auto state = TRY(validate(vm, generator_brand));

    // 2. If state is suspendedStart, then
    if (state == GeneratorState::SuspendedStart) {
        // a. Set generator.[[GeneratorState]] to completed.
        m_generator_state = GeneratorState::Completed;

        // b. Once a generator enters the completed state it never leaves it and its associated execution context is never resumed. Any execution state associated with generator can be discarded at this point.
        // We don't currently discard anything.

        // c. Set state to completed.
        state = GeneratorState::Completed;
    }

    // 3. If state is completed, then
    if (state == GeneratorState::Completed) {
        // a. If abruptCompletion.[[Type]] is return, then
        if (abrupt_completion.type() == Completion::Type::Return) {
            // i. Return CreateIterResultObject(abruptCompletion.[[Value]], true).
            return create_iterator_result_object(vm, abrupt_completion.value().value(), true);
        }

        // b. Return ? abruptCompletion.
        return abrupt_completion;
    }

    // 4. Assert: state is suspendedYield.
    VERIFY(state == GeneratorState::SuspendedYield);

    // 5. Let genContext be generator.[[GeneratorContext]].
    auto& generator_context = m_execution_context;

    // 6. Let methodContext be the running execution context.
    auto const& method_context = vm.running_execution_context();

    // 7. Suspend methodContext.
    // 9. Push genContext onto the execution context stack; genContext is now the running execution context.
    // NOTE: This is done out of order as to not permanently disable the generator if push_execution_context throws,
    //       as `resume_abrupt` will immediately throw when [[GeneratorState]] is "executing", never allowing the state to change.
    TRY(vm.push_execution_context(*generator_context, {}));

    // 8. Set generator.[[GeneratorState]] to executing.
    m_generator_state = GeneratorState::Executing;

    // 10. Resume the suspended evaluation of genContext using abruptCompletion as the result of the operation that suspended it. Let result be the Completion Record returned by the resumed computation.
    auto result = execute(vm, abrupt_completion);

    // 11. Assert: When we return here, genContext has already been removed from the execution context stack and methodContext is the currently running execution context.
    VERIFY(&vm.running_execution_context() == &method_context);

    // 12. Return ? result.
    return result;
}

}
