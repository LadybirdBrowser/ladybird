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
ThrowCompletionOr<NonnullGCPtr<Object>> ShadowRealmConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 2. Let O be ? OrdinaryCreateFromConstructor(NewTarget, "%ShadowRealm.prototype%", « [[ShadowRealm]], [[ExecutionContext]] »).
    auto object = TRY(ordinary_create_from_constructor<ShadowRealm>(vm, new_target, &Intrinsics::shadow_realm_prototype));

    // 3. Let realmRec be CreateRealm().
    auto realm = MUST_OR_THROW_OOM(Realm::create(vm));

    // 4. Set O.[[ShadowRealm]] to realmRec.
    object->set_shadow_realm(realm);

    // 5. Let context be a new execution context.
    auto context = ExecutionContext::create();

    // 6. Set the Function of context to null.
    context->function = nullptr;

    // 7. Set the Realm of context to realmRec.
    context->realm = realm;

    // 8. Set the ScriptOrModule of context to null.
    // Note: This is already the default value.

    // 9. Set O.[[ExecutionContext]] to context.
    object->set_execution_context(move(context));

    // 10. Perform ? SetRealmGlobalObject(realmRec, undefined, undefined).
    realm->set_global_object(nullptr, nullptr);

    // 11. Perform ? SetDefaultGlobalBindings(O.[[ShadowRealm]]).
    auto& global_object = set_default_global_bindings(object->shadow_realm());

    // FIXME: 12. Perform ? HostInitializeShadowRealm(O.[[ShadowRealm]]).
    global_object.initialize(object->shadow_realm());

    // 13. Return O.
    return object;
}

}
