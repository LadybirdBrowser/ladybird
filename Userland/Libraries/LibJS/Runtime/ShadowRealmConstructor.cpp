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

// https://github.com/tc39/proposal-shadowrealm/pull/392
static ThrowCompletionOr<NonnullGCPtr<Object>> initialize_shadow_realm(ShadowRealm& object)
{
    auto& vm = object.vm();

    // 1. Let context be the running Javascript execution context.
    auto& context = vm.running_execution_context();

    // 2. Let realm be the Realm of context.
    auto& realm = *context.realm;

    // 3. Return ? HostInitializeShadowRealm(realm, context, O).
    if (vm.host_initialize_shadow_realm)
        return TRY(vm.host_initialize_shadow_realm(realm, context.copy(), object));

    // AD-HOC: Fallback for when there is no host defined implementation.
    vm.pop_execution_context();
    object.set_execution_context(vm.running_execution_context().copy());
    object.set_shadow_realm(*vm.running_execution_context().realm);
    return Object::create(realm, realm.intrinsics().object_prototype());
}

// 3.2.1 ShadowRealm ( ), https://tc39.es/proposal-shadowrealm/#sec-shadowrealm
// https://github.com/tc39/proposal-shadowrealm/pull/392
ThrowCompletionOr<NonnullGCPtr<Object>> ShadowRealmConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 2. Let O be ? OrdinaryCreateFromConstructor(NewTarget, "%ShadowRealm.prototype%", « [[ShadowRealm]], [[ExecutionContext]] »).
    auto object = TRY(ordinary_create_from_constructor<ShadowRealm>(vm, new_target, &Intrinsics::shadow_realm_prototype));

    // 3. Perform ? InitializeHostDefinedRealm(). The customizations for creating the global object are to return ? InitializeShadowRealm().
    // 4. Let context be the running Javascript execution context.
    auto context = TRY(Realm::initialize_host_defined_realm(vm, [&object](JS::Realm&) -> JS::Object* { return MUST(initialize_shadow_realm(object)); }, nullptr));

    // 5. Set O.[[ExecutionContext]] to context.
    // 6. Let realmRec be the Realm of context.
    auto& realm_record = *context->realm;
    object->set_execution_context(move(context));

    // 7. Set O.[[ShadowRealm]] to realmRec.
    object->set_shadow_realm(realm_record);

    // 8. Return O.
    return object;
}

}
