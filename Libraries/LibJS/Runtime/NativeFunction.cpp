/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/AST.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/FunctionEnvironment.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

GC_DEFINE_ALLOCATOR(NativeFunction);

void NativeFunction::initialize(Realm& realm)
{
    Base::initialize(realm);
    m_name_string = PrimitiveString::create(vm(), m_name);
}

void NativeFunction::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit_possible_values(m_native_function.raw_capture_range());
    visitor.visit(m_realm);
    visitor.visit(m_name_string);
}

// 10.3.3 CreateBuiltinFunction ( behaviour, length, name, additionalInternalSlotsList [ , realm [ , prototype [ , prefix ] ] ] ), https://tc39.es/ecma262/#sec-createbuiltinfunction
// NOTE: This doesn't consider additionalInternalSlotsList, which is rarely used, and can either be implemented using only the `function` lambda, or needs a NativeFunction subclass.
GC::Ref<NativeFunction> NativeFunction::create(Realm& allocating_realm, Function<ThrowCompletionOr<Value>(VM&)> behaviour, i32 length, PropertyKey const& name, Optional<Realm*> realm, Optional<StringView> const& prefix, Optional<Bytecode::Builtin> builtin)
{
    auto& vm = allocating_realm.vm();

    // 1. If realm is not present, set realm to the current Realm Record.
    if (!realm.has_value())
        realm = vm.current_realm();

    // 2. If prototype is not present, set prototype to realm.[[Intrinsics]].[[%Function.prototype%]].
    auto prototype = realm.value()->intrinsics().function_prototype();

    // 3. Let internalSlotsList be a List containing the names of all the internal slots that 10.3 requires for the built-in function object that is about to be created.
    // 4. Append to internalSlotsList the elements of additionalInternalSlotsList.

    // 5. Let func be a new built-in function object that, when called, performs the action described by behaviour using the provided arguments as the values of the corresponding parameters specified by behaviour. The new function object has internal slots whose names are the elements of internalSlotsList, and an [[InitialName]] internal slot.
    // 6. Set func.[[Prototype]] to prototype.
    // 7. Set func.[[Extensible]] to true.
    // 8. Set func.[[Realm]] to realm.
    // 9. Set func.[[InitialName]] to null.
    auto function = allocating_realm.create<NativeFunction>(move(behaviour), prototype, *realm.value(), builtin);

    function->unsafe_set_shape(realm.value()->intrinsics().native_function_shape());

    // 10. Perform SetFunctionLength(func, length).
    function->put_direct(realm.value()->intrinsics().native_function_length_offset(), Value { length });

    // 11. If prefix is not present, then
    //     a. Perform SetFunctionName(func, name).
    // 12. Else,
    //     a. Perform SetFunctionName(func, name, prefix).
    function->put_direct(realm.value()->intrinsics().native_function_name_offset(), function->make_function_name(name, prefix));

    // 13. Return func.
    return function;
}

GC::Ref<NativeFunction> NativeFunction::create(Realm& realm, Utf16FlyString const& name, Function<ThrowCompletionOr<Value>(VM&)> function)
{
    return realm.create<NativeFunction>(name, move(function), realm.intrinsics().function_prototype());
}

NativeFunction::NativeFunction(AK::Function<ThrowCompletionOr<Value>(VM&)> native_function, Object* prototype, Realm& realm, Optional<Bytecode::Builtin> builtin)
    : FunctionObject(realm, prototype)
    , m_builtin(builtin)
    , m_native_function(move(native_function))
    , m_realm(&realm)
{
}

// FIXME: m_realm is supposed to be the realm argument of CreateBuiltinFunction, or the current
//        Realm Record. The former is not something that's commonly used or we support, the
//        latter is impossible as no ExecutionContext exists when most NativeFunctions are created...

NativeFunction::NativeFunction(Object& prototype)
    : FunctionObject(prototype)
    , m_realm(&prototype.shape().realm())
{
}

NativeFunction::NativeFunction(Utf16FlyString name, AK::Function<ThrowCompletionOr<Value>(VM&)> native_function, Object& prototype)
    : FunctionObject(prototype)
    , m_name(move(name))
    , m_native_function(move(native_function))
    , m_realm(&prototype.shape().realm())
{
}

NativeFunction::NativeFunction(Utf16FlyString name, Object& prototype)
    : FunctionObject(prototype)
    , m_name(move(name))
    , m_realm(&prototype.shape().realm())
{
}

// NOTE: Do not attempt to DRY these, it's not worth it. The difference in return types (Value vs Object*),
// called functions (call() vs construct(FunctionObject&)), and this value (passed vs uninitialized) make
// these good candidates for a bit of code duplication :^)

// 10.3.1 [[Call]] ( thisArgument, argumentsList ), https://tc39.es/ecma262/#sec-built-in-function-objects-call-thisargument-argumentslist
ThrowCompletionOr<Value> NativeFunction::internal_call(ExecutionContext& callee_context, Value this_argument)
{
    auto& vm = this->vm();

    // 1. Let callerContext be the running execution context.
    auto& caller_context = vm.running_execution_context();

    // 2. If callerContext is not already suspended, suspend callerContext.
    // 3. Let calleeContext be a new execution context.

    // 4. Set the Function of calleeContext to F.
    callee_context.function = this;

    // 5. Let calleeRealm be F.[[Realm]].
    auto callee_realm = m_realm;
    // NOTE: This non-standard fallback is needed until we can guarantee that literally
    // every function has a realm - especially in LibWeb that's sometimes not the case
    // when a function is created while no JS is running, as we currently need to rely on
    // that (:acid2:, I know - see set_event_handler_attribute() for an example).
    // If there's no 'current realm' either, we can't continue and crash.
    if (!callee_realm)
        callee_realm = vm.current_realm();
    VERIFY(callee_realm);

    // 6. Set the Realm of calleeContext to calleeRealm.
    callee_context.realm = callee_realm;

    // 7. Set the ScriptOrModule of calleeContext to null.
    // Note: This is already the default value.

    // 8. Perform any necessary implementation-defined initialization of calleeContext.
    callee_context.this_value = this_argument;

    callee_context.lexical_environment = caller_context.lexical_environment;
    callee_context.variable_environment = caller_context.variable_environment;
    // Note: Keeping the private environment is probably only needed because of async methods in classes
    //       calling async_block_start which goes through a NativeFunction here.
    callee_context.private_environment = caller_context.private_environment;

    // </8.> --------------------------------------------------------------------------

    // 9. Push calleeContext onto the execution context stack; calleeContext is now the running execution context.
    TRY(vm.push_execution_context(callee_context, {}));

    // 10. Let result be the Completion Record that is the result of evaluating F in a manner that conforms to the specification of F. thisArgument is the this value, argumentsList provides the named parameters, and the NewTarget value is undefined.
    auto result = call();

    // 11. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
    vm.pop_execution_context();

    // 12. Return ? result.
    return result;
}

// 10.3.2 [[Construct]] ( argumentsList, newTarget ), https://tc39.es/ecma262/#sec-built-in-function-objects-construct-argumentslist-newtarget
ThrowCompletionOr<GC::Ref<Object>> NativeFunction::internal_construct(ExecutionContext& callee_context, FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 1. Let callerContext be the running execution context.
    auto& caller_context = vm.running_execution_context();

    // 2. If callerContext is not already suspended, suspend callerContext.
    // 3. Let calleeContext be a new execution context.

    // 4. Set the Function of calleeContext to F.
    callee_context.function = this;

    // 5. Let calleeRealm be F.[[Realm]].
    auto callee_realm = m_realm;
    // NOTE: This non-standard fallback is needed until we can guarantee that literally
    // every function has a realm - especially in LibWeb that's sometimes not the case
    // when a function is created while no JS is running, as we currently need to rely on
    // that (:acid2:, I know - see set_event_handler_attribute() for an example).
    // If there's no 'current realm' either, we can't continue and crash.
    if (!callee_realm)
        callee_realm = vm.current_realm();
    VERIFY(callee_realm);

    // 6. Set the Realm of calleeContext to calleeRealm.
    callee_context.realm = callee_realm;

    // 7. Set the ScriptOrModule of calleeContext to null.
    // Note: This is already the default value.

    callee_context.lexical_environment = caller_context.lexical_environment;
    callee_context.variable_environment = caller_context.variable_environment;

    // </8.> --------------------------------------------------------------------------

    // 9. Push calleeContext onto the execution context stack; calleeContext is now the running execution context.
    TRY(vm.push_execution_context(callee_context, {}));

    // 10. Let result be the Completion Record that is the result of evaluating F in a manner that conforms to the specification of F. The this value is uninitialized, argumentsList provides the named parameters, and newTarget provides the NewTarget value.
    auto result = construct(new_target);

    // 11. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
    vm.pop_execution_context();

    // 12. Return ? result.
    return *TRY(result);
}

ThrowCompletionOr<Value> NativeFunction::call()
{
    VERIFY(m_native_function);
    return m_native_function(vm());
}

ThrowCompletionOr<GC::Ref<Object>> NativeFunction::construct(FunctionObject&)
{
    // Needs to be overridden if [[Construct]] is needed.
    VERIFY_NOT_REACHED();
}

bool NativeFunction::is_strict_mode() const
{
    return true;
}

Utf16String NativeFunction::name_for_call_stack() const
{
    return m_name.to_utf16_string();
}

}
