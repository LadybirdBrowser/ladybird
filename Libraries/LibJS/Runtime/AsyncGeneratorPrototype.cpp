/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AsyncGeneratorPrototype.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseConstructor.h>

namespace JS {

JS_DEFINE_ALLOCATOR(AsyncGeneratorPrototype);

// 27.6.1 Properties of the AsyncGenerator Prototype Object, https://tc39.es/ecma262/#sec-properties-of-asyncgenerator-prototype
AsyncGeneratorPrototype::AsyncGeneratorPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().async_iterator_prototype())
{
}

void AsyncGeneratorPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.next, next, 1, attr);
    define_native_function(realm, vm.names.return_, return_, 1, attr);
    define_native_function(realm, vm.names.throw_, throw_, 1, attr);

    // 27.6.1.5 AsyncGenerator.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-asyncgenerator-prototype-tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "AsyncGenerator"_string), Attribute::Configurable);
}

// 27.6.3.3 AsyncGeneratorValidate ( generator, generatorBrand ), https://tc39.es/ecma262/#sec-asyncgeneratorvalidate
static ThrowCompletionOr<NonnullGCPtr<AsyncGenerator>> async_generator_validate(VM& vm, Value generator, Optional<String> generator_brand)
{
    // 1. Perform ? RequireInternalSlot(generator, [[AsyncGeneratorContext]]).
    // 2. Perform ? RequireInternalSlot(generator, [[AsyncGeneratorState]]).
    // 3. Perform ? RequireInternalSlot(generator, [[AsyncGeneratorQueue]]).
    if (!generator.is_object() || !is<AsyncGenerator>(generator.as_object()))
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "AsyncGenerator");

    auto& async_generator = static_cast<AsyncGenerator&>(generator.as_object());

    // 4. If generator.[[GeneratorBrand]] is not generatorBrand, throw a TypeError exception.
    if (async_generator.generator_brand() != generator_brand)
        return vm.throw_completion<TypeError>(ErrorType::GeneratorBrandMismatch, async_generator.generator_brand().value_or("emp"_string), generator_brand.value_or("emp"_string));

    // 5. Return unused.
    return async_generator;
}

// 27.6.1.2 AsyncGenerator.prototype.next ( value ), https://tc39.es/ecma262/#sec-asyncgenerator-prototype-next
JS_DEFINE_NATIVE_FUNCTION(AsyncGeneratorPrototype::next)
{
    auto& realm = *vm.current_realm();

    // 1. Let generator be the this value.
    auto generator_this_value = vm.this_value();

    // 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
    auto promise_capability = MUST(new_promise_capability(vm, realm.intrinsics().promise_constructor()));

    // 3. Let result be Completion(AsyncGeneratorValidate(generator, empty)).
    // 4. IfAbruptRejectPromise(result, promiseCapability).
    auto generator = TRY_OR_REJECT(vm, promise_capability, async_generator_validate(vm, generator_this_value, OptionalNone {}));

    // 5. Let state be generator.[[AsyncGeneratorState]].
    auto state = generator->async_generator_state();

    // 6. If state is completed, then
    if (state == AsyncGenerator::State::Completed) {
        // a. Let iteratorResult be CreateIterResultObject(undefined, true).
        auto iterator_result = create_iterator_result_object(vm, js_undefined(), true);

        // b. Perform ! Call(promiseCapability.[[Resolve]], undefined, « iteratorResult »).
        MUST(call(vm, *promise_capability->resolve(), js_undefined(), iterator_result));

        // c. Return promiseCapability.[[Promise]].
        return promise_capability->promise();
    }

    // 7. Let completion be NormalCompletion(value).
    auto completion = normal_completion(vm.argument(0));

    // 8. Perform AsyncGeneratorEnqueue(generator, completion, promiseCapability).
    generator->async_generator_enqueue(completion, promise_capability);

    // 9. If state is either suspendedStart or suspendedYield, then
    if (state == AsyncGenerator::State::SuspendedStart || state == AsyncGenerator::State::SuspendedYield) {
        // a. Perform AsyncGeneratorResume(generator, completion).
        TRY_OR_REJECT(vm, promise_capability, generator->resume(vm, completion));
    }
    // 10. Else,
    else {
        // a. Assert: state is either executing or awaiting-return.
        VERIFY(state == AsyncGenerator::State::Executing || state == AsyncGenerator::State::AwaitingReturn);
    }

    // 11. Return promiseCapability.[[Promise]].
    return promise_capability->promise();
}

// 27.6.1.3 AsyncGenerator.prototype.return ( value ), https://tc39.es/ecma262/#sec-asyncgenerator-prototype-return
JS_DEFINE_NATIVE_FUNCTION(AsyncGeneratorPrototype::return_)
{
    auto& realm = *vm.current_realm();

    // 1. Let generator be the this value.
    auto generator_this_value = vm.this_value();

    // 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
    auto promise_capability = MUST(new_promise_capability(vm, realm.intrinsics().promise_constructor()));

    // 3. Let result be Completion(AsyncGeneratorValidate(generator, empty)).
    // 4. IfAbruptRejectPromise(result, promiseCapability).
    auto generator = TRY_OR_REJECT(vm, promise_capability, async_generator_validate(vm, generator_this_value, OptionalNone {}));

    // 5. Let completion be Completion Record { [[Type]]: return, [[Value]]: value, [[Target]]: empty }.
    auto completion = Completion(Completion::Type::Return, vm.argument(0));

    // 6. Perform AsyncGeneratorEnqueue(generator, completion, promiseCapability).
    generator->async_generator_enqueue(completion, promise_capability);

    // 7. Let state be generator.[[AsyncGeneratorState]].
    auto state = generator->async_generator_state();

    // 8. If state is either suspendedStart or completed, then
    if (state == AsyncGenerator::State::SuspendedStart || state == AsyncGenerator::State::Completed) {
        // a. Set generator.[[AsyncGeneratorState]] to awaiting-return.
        generator->set_async_generator_state({}, AsyncGenerator::State::AwaitingReturn);

        // b. Perform AsyncGeneratorAwaitReturn(generator).
        generator->await_return();
    }
    // 9. Else if state is suspendedYield, then
    else if (state == AsyncGenerator::State::SuspendedYield) {
        // a. Perform AsyncGeneratorResume(generator, completion).
        TRY_OR_REJECT(vm, promise_capability, generator->resume(vm, completion));
    }
    // 10. Else,
    else {
        // a. Assert: state is either executing or awaiting-return.
        VERIFY(state == AsyncGenerator::State::Executing || state == AsyncGenerator::State::AwaitingReturn);
    }

    // 11. Return promiseCapability.[[Promise]].
    return promise_capability->promise();
}

// 27.6.1.4 AsyncGenerator.prototype.throw ( exception ), https://tc39.es/ecma262/#sec-asyncgenerator-prototype-throw
JS_DEFINE_NATIVE_FUNCTION(AsyncGeneratorPrototype::throw_)
{
    auto& realm = *vm.current_realm();

    auto exception = vm.argument(0);

    // 1. Let generator be the this value.
    auto generator_this_value = vm.this_value();

    // 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
    auto promise_capability = MUST(new_promise_capability(vm, realm.intrinsics().promise_constructor()));

    // 3. Let result be Completion(AsyncGeneratorValidate(generator, empty)).
    // 4. IfAbruptRejectPromise(result, promiseCapability).
    auto generator = TRY_OR_REJECT(vm, promise_capability, async_generator_validate(vm, generator_this_value, OptionalNone {}));

    // 5. Let state be generator.[[AsyncGeneratorState]].
    auto state = generator->async_generator_state();

    // 6. If state is suspendedStart, then
    if (state == AsyncGenerator::State::SuspendedStart) {
        // a. Set generator.[[AsyncGeneratorState]] to completed.
        generator->set_async_generator_state({}, AsyncGenerator::State::Completed);

        // b. Set state to completed.
        state = AsyncGenerator::State::Completed;
    }

    // 7. If state is completed, then
    if (state == AsyncGenerator::State::Completed) {
        // a. Perform ! Call(promiseCapability.[[Reject]], undefined, « exception »).
        MUST(call(vm, *promise_capability->reject(), js_undefined(), exception));

        // b. Return promiseCapability.[[Promise]].
        return promise_capability->promise();
    }

    // 8. Let completion be ThrowCompletion(exception).
    auto completion = throw_completion(exception);

    // 9. Perform AsyncGeneratorEnqueue(generator, completion, promiseCapability).
    generator->async_generator_enqueue(completion, promise_capability);

    // 10. If state is suspendedYield, then
    if (state == AsyncGenerator::State::SuspendedYield) {
        // a. Perform AsyncGeneratorResume(generator, completion).
        TRY_OR_REJECT(vm, promise_capability, generator->resume(vm, completion));
    }
    // 11. Else,
    else {
        // a. Assert: state is either executing or awaiting-return.
        VERIFY(state == AsyncGenerator::State::Executing || state == AsyncGenerator::State::AwaitingReturn);
    }

    // 12. Return promiseCapability.[[Promise]].
    return promise_capability->promise();
}

}
