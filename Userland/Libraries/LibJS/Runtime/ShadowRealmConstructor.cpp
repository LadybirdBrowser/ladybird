/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ShadowRealm.h>
#include <LibJS/Runtime/ShadowRealmConstructor.h>

namespace JS {

JS_DEFINE_ALLOCATOR(ShadowRealmConstructor);

// 3.2 The ShadowRealm Constructor, https://tc39.es/proposal-shadowrealm/#sec-shadowrealm-constructor
ShadowRealmConstructor::ShadowRealmConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.ShadowRealm.as_string(), realm.intrinsics().function_prototype())
{
}

void ShadowRealmConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 3.3.1 ShadowRealm.prototype, https://tc39.es/proposal-shadowrealm/#sec-shadowrealm.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().shadow_realm_prototype(), 0);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
}

// 3.2.1 ShadowRealm ( ), https://tc39.es/proposal-shadowrealm/#sec-shadowrealm
ThrowCompletionOr<Value> ShadowRealmConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, vm.names.ShadowRealm);
}

// 3.2.1 ShadowRealm ( ), https://tc39.es/proposal-shadowrealm/#sec-shadowrealm
// https://github.com/tc39/proposal-shadowrealm/pull/410
ThrowCompletionOr<NonnullGCPtr<Object>> ShadowRealmConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 2. Let O be ? OrdinaryCreateFromConstructor(NewTarget, "%ShadowRealm.prototype%", « [[ShadowRealm]] »).
    auto object = TRY(ordinary_create_from_constructor<ShadowRealm>(vm, new_target, &Intrinsics::shadow_realm_prototype));

    // 3. Let callerContext be the running execution context.
    // 4. Perform ? InitializeHostDefinedRealm().
    // 5. Let innerContext be the running execution context.
    auto inner_context = TRY(Realm::initialize_host_defined_realm(vm, nullptr, nullptr));

    // 6. Remove innerContext from the execution context stack and restore callerContext as the running execution context.
    vm.pop_execution_context();

    // 7. Let realmRec be the Realm of innerContext.
    auto& realm_record = *inner_context->realm;

    // 8. Set O.[[ShadowRealm]] to realmRec.
    object->set_shadow_realm(realm_record);

    // 9. Perform ? HostInitializeShadowRealm(realmRec).
    TRY(vm.host_initialize_shadow_realm(realm_record, move(inner_context), object));

    // 10. Return O.
    return object;
}

}
