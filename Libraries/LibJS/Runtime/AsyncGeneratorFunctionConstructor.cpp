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

GC_DEFINE_ALLOCATOR(AsyncGeneratorFunctionConstructor);

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
ThrowCompletionOr<GC::Ref<Object>> AsyncGeneratorFunctionConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    ReadonlySpan<Value> arguments = vm.running_execution_context().arguments;

    ReadonlySpan<Value> parameter_args = arguments;
    if (!parameter_args.is_empty())
        parameter_args = parameter_args.slice(0, parameter_args.size() - 1);

    // 1. Let C be the active function object.
    auto* constructor = vm.active_function_object();

    // 2. If bodyArg is not present, set bodyArg to the empty String.
    Value body_arg = vm.empty_string();
    if (!arguments.is_empty())
        body_arg = arguments.last();

    // 3. Return ? CreateDynamicFunction(C, NewTarget, async-generator, parameterArgs, bodyArg).
    return TRY(FunctionConstructor::create_dynamic_function(vm, *constructor, &new_target, FunctionKind::AsyncGenerator, parameter_args, body_arg));
}

}
