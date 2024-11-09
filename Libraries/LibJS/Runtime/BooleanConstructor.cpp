/*
 * Copyright (c) 2020, Jack Karamanian <karamanian.jack@gmail.com>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/BooleanConstructor.h>
#include <LibJS/Runtime/BooleanObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

JS_DEFINE_ALLOCATOR(BooleanConstructor);

BooleanConstructor::BooleanConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Boolean.as_string(), realm.intrinsics().function_prototype())
{
}

void BooleanConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 20.3.2.1 Boolean.prototype, https://tc39.es/ecma262/#sec-boolean.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().boolean_prototype(), 0);

    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
}

// 20.3.1.1 Boolean ( value ), https://tc39.es/ecma262/#sec-boolean-constructor-boolean-value
ThrowCompletionOr<Value> BooleanConstructor::call()
{
    auto& vm = this->vm();
    auto value = vm.argument(0);

    // 1. Let b be ToBoolean(value).
    auto b = value.to_boolean();

    // 2. If NewTarget is undefined, return b.
    return Value(b);
}

// 20.3.1.1 Boolean ( value ), https://tc39.es/ecma262/#sec-boolean-constructor-boolean-value
ThrowCompletionOr<NonnullGCPtr<Object>> BooleanConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();
    auto value = vm.argument(0);

    // 1. Let b be ToBoolean(value).
    auto b = value.to_boolean();

    // 3. Let O be ? OrdinaryCreateFromConstructor(NewTarget, "%Boolean.prototype%", « [[BooleanData]] »).
    // 4. Set O.[[BooleanData]] to b.
    // 5. Return O.
    return TRY(ordinary_create_from_constructor<BooleanObject>(vm, new_target, &Intrinsics::boolean_prototype, b));
}

}
