/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/WeakRef.h>
#include <LibJS/Runtime/WeakRefConstructor.h>

namespace JS {

JS_DEFINE_ALLOCATOR(WeakRefConstructor);

WeakRefConstructor::WeakRefConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.WeakRef.as_string(), realm.intrinsics().function_prototype())
{
}

void WeakRefConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 26.1.2.1 WeakRef.prototype, https://tc39.es/ecma262/#sec-weak-ref.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().weak_ref_prototype(), 0);

    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
}

// 26.1.1.1 WeakRef ( target ), https://tc39.es/ecma262/#sec-weak-ref-target
ThrowCompletionOr<Value> WeakRefConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, vm.names.WeakRef);
}

// 26.1.1.1 WeakRef ( target ), https://tc39.es/ecma262/#sec-weak-ref-target
ThrowCompletionOr<NonnullGCPtr<Object>> WeakRefConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();
    auto target = vm.argument(0);

    // 2. If CanBeHeldWeakly(target) is false, throw a TypeError exception.
    if (!can_be_held_weakly(target))
        return vm.throw_completion<TypeError>(ErrorType::CannotBeHeldWeakly, target.to_string_without_side_effects());

    // 3. Let weakRef be ? OrdinaryCreateFromConstructor(NewTarget, "%WeakRef.prototype%", « [[WeakRefTarget]] »).
    // 4. Perform AddToKeptObjects(target).
    // 5. Set weakRef.[[WeakRefTarget]] to target.
    // 6. Return weakRef.
    if (target.is_object())
        return TRY(ordinary_create_from_constructor<WeakRef>(vm, new_target, &Intrinsics::weak_ref_prototype, target.as_object()));
    VERIFY(target.is_symbol());
    return TRY(ordinary_create_from_constructor<WeakRef>(vm, new_target, &Intrinsics::weak_ref_prototype, target.as_symbol()));
}

}
