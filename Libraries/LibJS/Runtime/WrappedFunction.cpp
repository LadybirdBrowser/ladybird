/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ShadowRealm.h>
#include <LibJS/Runtime/WrappedFunction.h>

namespace JS {

GC_DEFINE_ALLOCATOR(WrappedFunction);

// 3.1.1 WrappedFunctionCreate ( callerRealm: a Realm Record, Target: a function object, ), https://tc39.es/proposal-shadowrealm/#sec-wrappedfunctioncreate
ThrowCompletionOr<GC::Ref<WrappedFunction>> WrappedFunction::create(Realm& realm, Realm& caller_realm, FunctionObject& target)
{
    auto& vm = realm.vm();

    // 1. Let internalSlotsList be the internal slots listed in Table 2, plus [[Prototype]] and [[Extensible]].
    // 2. Let wrapped be MakeBasicObject(internalSlotsList).
    // 3. Set wrapped.[[Prototype]] to callerRealm.[[Intrinsics]].[[%Function.prototype%]].
    // 4. Set wrapped.[[Call]] as described in 2.1.
    // 5. Set wrapped.[[WrappedTargetFunction]] to Target.
    // 6. Set wrapped.[[Realm]] to callerRealm.
    auto& prototype = *caller_realm.intrinsics().function_prototype();
    auto wrapped = realm.create<WrappedFunction>(caller_realm, target, prototype);

    // 7. Let result be CopyNameAndLength(wrapped, Target).
    auto result = copy_name_and_length(vm, *wrapped, target);

    // 8. If result is an Abrupt Completion, throw a TypeError exception.
    if (result.is_throw_completion())
        return vm.throw_completion<TypeError>(ErrorType::WrappedFunctionCopyNameAndLengthThrowCompletion);

    // 9. Return wrapped.
    return wrapped;
}

// 2 Wrapped Function Exotic Objects, https://tc39.es/proposal-shadowrealm/#sec-wrapped-function-exotic-objects
WrappedFunction::WrappedFunction(Realm& realm, FunctionObject& wrapped_target_function, Object& prototype)
    : FunctionObject(prototype)
    , m_wrapped_target_function(wrapped_target_function)
    , m_realm(realm)
{
}

void WrappedFunction::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_wrapped_target_function);
    visitor.visit(m_realm);
}

// 2.1 [[Call]] ( thisArgument, argumentsList ), https://tc39.es/proposal-shadowrealm/#sec-wrapped-function-exotic-objects-call-thisargument-argumentslist
ThrowCompletionOr<Value> WrappedFunction::internal_call(ExecutionContext& callee_context, Value this_argument)
{
    auto& vm = this->vm();

    // 1. Let callerContext be the running execution context.
    // NOTE: No-op, kept by the VM in its execution context stack.

    // 2. Let calleeContext be PrepareForWrappedFunctionCall(F).
    prepare_for_wrapped_function_call(*this, callee_context);

    // 3. Assert: calleeContext is now the running execution context.
    VERIFY(&vm.running_execution_context() == &callee_context);

    // 4. Let result be Completion(OrdinaryWrappedFunctionCall(F, thisArgument, argumentsList)).
    auto result = ordinary_wrapped_function_call(*this, this_argument, callee_context.arguments);

    // 5. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
    vm.pop_execution_context();

    // 6. Return ? result.
    return result;
}

ThrowCompletionOr<void> WrappedFunction::get_stack_frame_size(size_t& registers_and_constants_and_locals_count, size_t& argument_count)
{
    return m_wrapped_target_function->get_stack_frame_size(registers_and_constants_and_locals_count, argument_count);
}

// 2.2 OrdinaryWrappedFunctionCall ( F: a wrapped function exotic object, thisArgument: an ECMAScript language value, argumentsList: a List of ECMAScript language values, ), https://tc39.es/proposal-shadowrealm/#sec-ordinary-wrapped-function-call
ThrowCompletionOr<Value> ordinary_wrapped_function_call(WrappedFunction& function, Value this_argument, Span<Value> arguments_list)
{
    auto& vm = function.vm();

    // 1. Let target be F.[[WrappedTargetFunction]].
    auto& target = function.wrapped_target_function();

    // 2. Assert: IsCallable(target) is true.
    VERIFY(Value(&target).is_function());

    // 3. Let callerRealm be F.[[Realm]].
    auto* caller_realm = function.realm();

    // 4. NOTE: Any exception objects produced after this point are associated with callerRealm.
    VERIFY(vm.current_realm() == caller_realm);

    // 5. Let targetRealm be ? GetFunctionRealm(target).
    auto* target_realm = TRY(get_function_realm(vm, target));

    // 6. Let wrappedArgs be a new empty List.
    auto wrapped_args = GC::RootVector<Value> { vm.heap() };
    wrapped_args.ensure_capacity(arguments_list.size());

    // 7. For each element arg of argumentsList, do
    for (auto const& arg : arguments_list) {
        // a. Let wrappedValue be ? GetWrappedValue(targetRealm, arg).
        auto wrapped_value = TRY(get_wrapped_value(vm, *target_realm, arg));

        // b. Append wrappedValue to wrappedArgs.
        wrapped_args.append(wrapped_value);
    }

    // 8. Let wrappedThisArgument to ? GetWrappedValue(targetRealm, thisArgument).
    auto wrapped_this_argument = TRY(get_wrapped_value(vm, *target_realm, this_argument));

    // 9. Let result be the Completion Record of Call(target, wrappedThisArgument, wrappedArgs).
    auto result = call(vm, &target, wrapped_this_argument, wrapped_args.span());

    // 10. If result.[[Type]] is normal or result.[[Type]] is return, then
    if (!result.is_throw_completion()) {
        // a. Return ? GetWrappedValue(callerRealm, result.[[Value]]).
        return get_wrapped_value(vm, *caller_realm, result.value());
    }
    // 11. Else,
    else {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::WrappedFunctionCallThrowCompletion);
    }

    // NOTE: Also see "Editor's Note" in the spec regarding the TypeError above.
}

// 2.3 PrepareForWrappedFunctionCall ( F: a wrapped function exotic object, ), https://tc39.es/proposal-shadowrealm/#sec-prepare-for-wrapped-function-call
void prepare_for_wrapped_function_call(WrappedFunction& function, ExecutionContext& callee_context)
{
    auto& vm = function.vm();

    // 1. Let callerContext be the running execution context.
    auto const& caller_context = vm.running_execution_context();

    // 2. Let calleeContext be a new execution context.

    // NOTE: In the specification, PrepareForWrappedFunctionCall "returns" a new callee execution context.
    // To avoid heap allocations, we put our ExecutionContext objects on the C++ stack instead.
    // Whoever calls us should put an ExecutionContext on their stack and pass that as the `callee_context`.

    // 3. Set the Function of calleeContext to F.
    callee_context.function = &const_cast<WrappedFunction&>(function);

    // 4. Let calleeRealm be F.[[Realm]].
    auto* callee_realm = function.realm();

    // 5. Set the Realm of calleeContext to calleeRealm.
    callee_context.realm = callee_realm;

    // 6. Set the ScriptOrModule of calleeContext to null.
    callee_context.script_or_module = {};

    // 7. If callerContext is not already suspended, suspend callerContext.
    // NOTE: We don't support this concept yet.
    (void)caller_context;

    // 8. Push calleeContext onto the execution context stack; calleeContext is now the running execution context.
    vm.push_execution_context(callee_context);

    // 9. NOTE: Any exception objects produced after this point are associated with calleeRealm.

    // 10. Return calleeContext.
    // NOTE: No-op, see NOTE after step 2.
}

Utf16String WrappedFunction::name_for_call_stack() const
{
    return "(Wrapped)"_utf16;
}

}
