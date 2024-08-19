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

GC_DEFINE_ALLOCATOR(AsyncFunctionConstructor);

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

// 27.7.1.1 AsyncFunction ( p1, p2, … , pn, body ), https://tc39.es/ecma262/#sec-async-function-constructor-arguments
ThrowCompletionOr<GC::Ref<Object>> AsyncFunctionConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 1. Let C be the active function object.
    auto* constructor = vm.active_function_object();

    // 2. Let args be the argumentsList that was passed to this function by [[Call]] or [[Construct]].
    GC::MarkedVector<Value> args(heap());
    for (auto argument : vm.running_execution_context().arguments)
        args.append(argument);

    // 3. Return CreateDynamicFunction(C, NewTarget, async, args).
    return *TRY(FunctionConstructor::create_dynamic_function(vm, *constructor, &new_target, FunctionKind::Async, args));
}

}
