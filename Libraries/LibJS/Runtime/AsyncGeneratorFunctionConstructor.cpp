/*
 * Copyright (c) 2021, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AsyncGeneratorFunctionConstructor.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/FunctionConstructor.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

JS_DEFINE_ALLOCATOR(AsyncGeneratorFunctionConstructor);

AsyncGeneratorFunctionConstructor::AsyncGeneratorFunctionConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.AsyncGeneratorFunction.as_string(), realm.intrinsics().function_prototype())
{
}

void AsyncGeneratorFunctionConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 27.4.2.1 AsyncGeneratorFunction.length, https://tc39.es/ecma262/#sec-asyncgeneratorfunction-length
    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);

    // 27.4.2.2 AsyncGeneratorFunction.prototype, https://tc39.es/ecma262/#sec-asyncgeneratorfunction-prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().async_generator_function_prototype(), 0);
}

// 27.4.1.1 AsyncGeneratorFunction ( p1, p2, … , pn, body ), https://tc39.es/ecma262/#sec-asyncgeneratorfunction
ThrowCompletionOr<Value> AsyncGeneratorFunctionConstructor::call()
{
    return TRY(construct(*this));
}

// 27.4.1.1 AsyncGeneratorFunction ( ...parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-asyncgeneratorfunction
ThrowCompletionOr<NonnullGCPtr<Object>> AsyncGeneratorFunctionConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 1. Let C be the active function object.
    auto* constructor = vm.active_function_object();

    // 2. If bodyArg is not present, set bodyArg to the empty String.
    // NOTE: This does that, as well as the string extraction done inside of CreateDynamicFunction
    auto extracted = TRY(extract_parameter_arguments_and_body(vm, vm.running_execution_context().arguments));

    // 3. Return ? CreateDynamicFunction(C, NewTarget, async-generator, parameterArgs, bodyArg).
    return TRY(FunctionConstructor::create_dynamic_function(vm, *constructor, &new_target, FunctionKind::AsyncGenerator, extracted.parameters, extracted.body));
}

}
