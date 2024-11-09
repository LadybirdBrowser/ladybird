/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AsyncFunctionConstructor.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/FunctionConstructor.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

JS_DEFINE_ALLOCATOR(AsyncFunctionConstructor);

AsyncFunctionConstructor::AsyncFunctionConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.AsyncFunction.as_string(), realm.intrinsics().function_constructor())
{
}

void AsyncFunctionConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 27.7.2.2 AsyncFunction.prototype, https://tc39.es/ecma262/#sec-async-function-constructor-prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().async_function_prototype(), 0);

    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
}

// 27.7.1.1 AsyncFunction ( p1, p2, … , pn, body ), https://tc39.es/ecma262/#sec-async-function-constructor-arguments
ThrowCompletionOr<Value> AsyncFunctionConstructor::call()
{
    return TRY(construct(*this));
}

// 27.7.1.1 AsyncFunction ( ...parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-async-function-constructor-arguments
ThrowCompletionOr<NonnullGCPtr<Object>> AsyncFunctionConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 1. Let C be the active function object.
    auto* constructor = vm.active_function_object();

    // 2. If bodyArg is not present, set bodyArg to the empty String.
    // NOTE: This does that, as well as the string extraction done inside of CreateDynamicFunction
    auto extracted = TRY(extract_parameter_arguments_and_body(vm, vm.running_execution_context().arguments));

    // 3. Return ? CreateDynamicFunction(C, NewTarget, async, parameterArgs, bodyArg).
    return TRY(FunctionConstructor::create_dynamic_function(vm, *constructor, &new_target, FunctionKind::Async, extracted.parameters, extracted.body));
}

}
