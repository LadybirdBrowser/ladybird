/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Heap/DeferGC.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

JS_DEFINE_ALLOCATOR(Realm);

// 9.3.1 CreateRealm ( ), https://tc39.es/ecma262/#sec-createrealm
ThrowCompletionOr<NonnullGCPtr<Realm>> Realm::create(VM& vm)
{
    // 1. Let realmRec be a new Realm Record.
    auto realm = vm.heap().allocate_without_realm<Realm>();

    // 2. Perform CreateIntrinsics(realmRec).
    MUST_OR_THROW_OOM(Intrinsics::create(*realm));

    // 3. Set realmRec.[[GlobalObject]] to undefined.
    // 4. Set realmRec.[[GlobalEnv]] to undefined.
    // 5. Set realmRec.[[TemplateMap]] to a new empty List.

    // 6. Return realmRec.
    return realm;
}

// 9.6 InitializeHostDefinedRealm ( ), https://tc39.es/ecma262/#sec-initializehostdefinedrealm
ThrowCompletionOr<NonnullOwnPtr<ExecutionContext>> Realm::initialize_host_defined_realm(VM& vm, Function<Object*(Realm&)> create_global_object, Function<Object*(Realm&)> create_global_this_value)
{
    DeferGC defer_gc(vm.heap());

    // 1. Let realm be CreateRealm().
    auto realm = MUST_OR_THROW_OOM(Realm::create(vm));

    // 2. Let newContext be a new execution context.
    auto new_context = ExecutionContext::create();

    // 3. Set the Function of newContext to null.
    new_context->function = nullptr;

    // 4. Set the Realm of newContext to realm.
    new_context->realm = realm;

    // 5. Set the ScriptOrModule of newContext to null.
    new_context->script_or_module = {};

    // 6. Push newContext onto the execution context stack; newContext is now the running execution context.
    vm.push_execution_context(*new_context);

    // 7. If the host requires use of an exotic object to serve as realm's global object,
    //    let global be such an object created in a host-defined manner.
    //    Otherwise, let global be undefined, indicating that an ordinary object should be created as the global object.
    Object* global = nullptr;
    if (create_global_object)
        global = create_global_object(*realm);

    // 8. If the host requires that the this binding in realm's global scope return an object other than the global object,
    //    let thisValue be such an object created in a host-defined manner.
    //    Otherwise, let thisValue be undefined, indicating that realm's global this binding should be the global object.
    Object* this_value = nullptr;
    if (create_global_this_value)
        this_value = create_global_this_value(*realm);

    // 9. Perform SetRealmGlobalObject(realm, global, thisValue).
    realm->set_global_object(global, this_value);

    // 10. Let globalObj be ? SetDefaultGlobalBindings(realm).
    auto& global_object = set_default_global_bindings(*realm);

    // 11. Create any host-defined global object properties on globalObj.
    global_object.initialize(*realm);

    // 12. Return unused.
    return new_context;
}

// 9.3.3 SetRealmGlobalObject ( realmRec, globalObj, thisValue ), https://tc39.es/ecma262/#sec-setrealmglobalobject
void Realm::set_global_object(Object* global_object, Object* this_value)
{
    // 1. If globalObj is undefined, then
    if (global_object == nullptr) {
        // a. Let intrinsics be realmRec.[[Intrinsics]].
        // b. Set globalObj to OrdinaryObjectCreate(intrinsics.[[%Object.prototype%]]).
        // NOTE: We allocate a proper GlobalObject directly as this plain object is
        //       turned into one via SetDefaultGlobalBindings in the spec.
        global_object = heap().allocate_without_realm<GlobalObject>(*this);
    }

    // 2. Assert: Type(globalObj) is Object.
    VERIFY(global_object);

    // 3. If thisValue is undefined, set thisValue to globalObj.
    if (this_value == nullptr)
        this_value = global_object;

    // Non-standard
    VERIFY(this_value);

    // 4. Set realmRec.[[GlobalObject]] to globalObj.
    m_global_object = global_object;

    // 5. Let newGlobalEnv be NewGlobalEnvironment(globalObj, thisValue).
    // 6. Set realmRec.[[GlobalEnv]] to newGlobalEnv.
    m_global_environment = m_global_object->heap().allocate_without_realm<GlobalEnvironment>(*global_object, *this_value);

    // 7. Return unused.
}

void Realm::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_intrinsics);
    visitor.visit(m_global_object);
    visitor.visit(m_global_environment);
    if (m_host_defined)
        m_host_defined->visit_edges(visitor);
}

}
