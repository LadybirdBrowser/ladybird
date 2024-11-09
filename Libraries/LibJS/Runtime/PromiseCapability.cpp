/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PromiseCapability.h>

namespace JS {

JS_DEFINE_ALLOCATOR(PromiseCapability);

NonnullGCPtr<PromiseCapability> PromiseCapability::create(VM& vm, NonnullGCPtr<Object> promise, NonnullGCPtr<FunctionObject> resolve, NonnullGCPtr<FunctionObject> reject)
{
    return vm.heap().allocate_without_realm<PromiseCapability>(promise, resolve, reject);
}

PromiseCapability::PromiseCapability(NonnullGCPtr<Object> promise, NonnullGCPtr<FunctionObject> resolve, NonnullGCPtr<FunctionObject> reject)
    : m_promise(promise)
    , m_resolve(resolve)
    , m_reject(reject)
{
}

void PromiseCapability::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_promise);
    visitor.visit(m_resolve);
    visitor.visit(m_reject);
}

namespace {
struct ResolvingFunctions final : public Cell {
    JS_CELL(ResolvingFunctions, Cell);
    JS_DECLARE_ALLOCATOR(ResolvingFunctions);

    Value resolve { js_undefined() };
    Value reject { js_undefined() };

    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(resolve);
        visitor.visit(reject);
    }
};
JS_DEFINE_ALLOCATOR(ResolvingFunctions);
}

// 27.2.1.5 NewPromiseCapability ( C ), https://tc39.es/ecma262/#sec-newpromisecapability
ThrowCompletionOr<NonnullGCPtr<PromiseCapability>> new_promise_capability(VM& vm, Value constructor)
{
    auto& realm = *vm.current_realm();

    // 1. If IsConstructor(C) is false, throw a TypeError exception.
    if (!constructor.is_constructor())
        return vm.throw_completion<TypeError>(ErrorType::NotAConstructor, constructor.to_string_without_side_effects());

    // 2. NOTE: C is assumed to be a constructor function that supports the parameter conventions of the Promise constructor (see 27.2.3.1).

    // 3. Let resolvingFunctions be the Record { [[Resolve]]: undefined, [[Reject]]: undefined }.
    auto resolving_functions = vm.heap().allocate<ResolvingFunctions>(realm);

    // 4. Let executorClosure be a new Abstract Closure with parameters (resolve, reject) that captures resolvingFunctions and performs the following steps when called:
    auto executor_closure = [resolving_functions](auto& vm) -> ThrowCompletionOr<Value> {
        auto resolve = vm.argument(0);
        auto reject = vm.argument(1);

        // No idea what other engines say here.
        // a. If promiseCapability.[[Resolve]] is not undefined, throw a TypeError exception.
        if (!resolving_functions->resolve.is_undefined())
            return vm.template throw_completion<TypeError>(ErrorType::GetCapabilitiesExecutorCalledMultipleTimes);

        // b. If promiseCapability.[[Reject]] is not undefined, throw a TypeError exception.
        if (!resolving_functions->reject.is_undefined())
            return vm.template throw_completion<TypeError>(ErrorType::GetCapabilitiesExecutorCalledMultipleTimes);

        // c. Set promiseCapability.[[Resolve]] to resolve.
        resolving_functions->resolve = resolve;

        // d. Set promiseCapability.[[Reject]] to reject.
        resolving_functions->reject = reject;

        // e. Return undefined.
        return js_undefined();
    };

    // 5. Let executor be CreateBuiltinFunction(executorClosure, 2, "", « »).
    auto executor = NativeFunction::create(realm, move(executor_closure), 2, "");

    // 6. Let promise be ? Construct(C, « executor »).
    auto promise = TRY(construct(vm, constructor.as_function(), executor));

    //  7. If IsCallable(resolvingFunctions.[[Resolve]]) is false, throw a TypeError exception.
    if (!resolving_functions->resolve.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, "Promise capability resolve value");

    // 8. If IsCallable(resolvingFunctions.[[Reject]]) is false, throw a TypeError exception.
    if (!resolving_functions->reject.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, "Promise capability reject value");

    // 9. Return the PromiseCapability Record { [[Promise]]: promise, [[Resolve]]: resolvingFunctions.[[Resolve]], [[Reject]]: resolvingFunctions.[[Reject]] }.
    return PromiseCapability::create(vm, promise, resolving_functions->resolve.as_function(), resolving_functions->reject.as_function());
}

}
