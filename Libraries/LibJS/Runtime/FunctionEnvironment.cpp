/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/FunctionEnvironment.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

JS_DEFINE_ALLOCATOR(FunctionEnvironment);

FunctionEnvironment::FunctionEnvironment(Environment* parent_environment)
    : DeclarativeEnvironment(parent_environment)
{
}

void FunctionEnvironment::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_this_value);
    visitor.visit(m_new_target);
    visitor.visit(m_function_object);
}

// 9.1.1.3.5 GetSuperBase ( ), https://tc39.es/ecma262/#sec-getsuperbase
ThrowCompletionOr<Value> FunctionEnvironment::get_super_base() const
{
    VERIFY(m_function_object);

    // 1. Let home be envRec.[[FunctionObject]].[[HomeObject]].
    auto home_object = m_function_object->home_object();

    // 2. If home is undefined, return undefined.
    if (!home_object)
        return js_undefined();

    // 3. Assert: Type(home) is Object.

    // 4. Return ? home.[[GetPrototypeOf]]().
    return TRY(home_object->internal_get_prototype_of());
}

// 9.1.1.3.2 HasThisBinding ( ), https://tc39.es/ecma262/#sec-function-environment-records-hasthisbinding
bool FunctionEnvironment::has_this_binding() const
{
    if (this_binding_status() == ThisBindingStatus::Lexical)
        return false;
    return true;
}

// 9.1.1.3.3 HasSuperBinding ( ), https://tc39.es/ecma262/#sec-function-environment-records-hassuperbinding
bool FunctionEnvironment::has_super_binding() const
{
    if (this_binding_status() == ThisBindingStatus::Lexical)
        return false;
    if (!function_object().home_object())
        return false;
    return true;
}

// 9.1.1.3.4 GetThisBinding ( ), https://tc39.es/ecma262/#sec-function-environment-records-getthisbinding
ThrowCompletionOr<Value> FunctionEnvironment::get_this_binding(VM& vm) const
{
    // 1. Assert: envRec.[[ThisBindingStatus]] is not lexical.
    VERIFY(m_this_binding_status != ThisBindingStatus::Lexical);

    // 2. If envRec.[[ThisBindingStatus]] is uninitialized, throw a ReferenceError exception.
    if (m_this_binding_status == ThisBindingStatus::Uninitialized)
        return vm.throw_completion<ReferenceError>(ErrorType::ThisHasNotBeenInitialized);

    // 3. Return envRec.[[ThisValue]].
    return m_this_value;
}

// 9.1.1.3.1 BindThisValue ( V ), https://tc39.es/ecma262/#sec-bindthisvalue
ThrowCompletionOr<Value> FunctionEnvironment::bind_this_value(VM& vm, Value this_value)
{
    VERIFY(!this_value.is_empty());

    // 1. Assert: envRec.[[ThisBindingStatus]] is not lexical.
    VERIFY(m_this_binding_status != ThisBindingStatus::Lexical);

    // 2. If envRec.[[ThisBindingStatus]] is initialized, throw a ReferenceError exception.
    if (m_this_binding_status == ThisBindingStatus::Initialized)
        return vm.throw_completion<ReferenceError>(ErrorType::ThisIsAlreadyInitialized);

    // 3. Set envRec.[[ThisValue]] to V.
    m_this_value = this_value;

    // 4. Set envRec.[[ThisBindingStatus]] to initialized.
    m_this_binding_status = ThisBindingStatus::Initialized;

    // 5. Return V.
    return this_value;
}

}
