/*
 * Copyright (c) 2021, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/FunctionConstructor.h>
#include <LibJS/Runtime/GeneratorFunctionConstructor.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

GC_DEFINE_ALLOCATOR(GeneratorFunctionConstructor);

GeneratorFunctionConstructor::GeneratorFunctionConstructor(Realm& realm)
    : NativeFunction(static_cast<Object&>(realm.intrinsics().function_constructor()))
{
}

void GeneratorFunctionConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 27.3.2.1 GeneratorFunction.length, https://tc39.es/ecma262/#sec-generatorfunction.length
    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
    // 27.3.2.2 GeneratorFunction.prototype, https://tc39.es/ecma262/#sec-generatorfunction.length
    define_direct_property(vm.names.prototype, realm.intrinsics().generator_function_prototype(), 0);
}

// 27.3.1.1 GeneratorFunction ( p1, p2, … , pn, body ), https://tc39.es/ecma262/#sec-generatorfunction
ThrowCompletionOr<Value> GeneratorFunctionConstructor::call()
{
    return TRY(construct(*this));
}

// 27.3.1.1 GeneratorFunction ( ...parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-generatorfunction
ThrowCompletionOr<GC::Ref<Object>> GeneratorFunctionConstructor::construct(FunctionObject& new_target)
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

    // 3. Return ? CreateDynamicFunction(C, NewTarget, generator, parameterArgs, bodyArg).
    return TRY(FunctionConstructor::create_dynamic_function(vm, *constructor, &new_target, FunctionKind::Generator, parameter_args, body_arg));
}

}
