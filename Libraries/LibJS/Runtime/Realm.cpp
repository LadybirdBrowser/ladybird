/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibGC/DeferGC.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

GC_DEFINE_ALLOCATOR(Realm);

// 9.3.1 InitializeHostDefinedRealm ( ), https://tc39.es/ecma262/#sec-initializehostdefinedrealm
ThrowCompletionOr<NonnullOwnPtr<ExecutionContext>> Realm::initialize_host_defined_realm(VM& vm, Function<Object*(Realm&)> create_global_object, Function<Object*(Realm&)> create_global_this_value)
{
    GC::DeferGC defer_gc(vm.heap());

    // 1. Let realm be a new Realm Record
    auto realm = vm.heap().allocate<Realm>();

    // 2. Perform CreateIntrinsics(realm).
    Intrinsics::create(*realm);

    // FIXME: 3. Set realm.[[AgentSignifier]] to AgentSignifier().

    // NOTE: Done on step 1.
    // 4. Set realm.[[GlobalObject]] to undefined.
    // 5. Set realm.[[GlobalEnv]] to undefined.

    // FIXME: 6. Set realm.[[TemplateMap]] to a new empty List.

    // 7. Let newContext be a new execution context.
    auto new_context = ExecutionContext::create();

    // 8. Set the Function of newContext to null.
    new_context->function = nullptr;

    // 9. Set the Realm of newContext to realm.
    new_context->realm = realm;

    // 10. Set the ScriptOrModule of newContext to null.
    new_context->script_or_module = {};

    // 11. Push newContext onto the execution context stack; newContext is now the running execution context.
    vm.push_execution_context(*new_context);

    // 12. If the host requires use of an exotic object to serve as realm's global object, then
    Object* global = nullptr;
    if (create_global_object) {
        // a. Let global be such an object created in a host-defined manner.
        global = create_global_object(*realm);
    }
    // 13. Else,
    else {
        // a. Let global be OrdinaryObjectCreate(realm.[[Intrinsics]].[[%Object.prototype%]]).
        // NOTE: We allocate a proper GlobalObject directly as this plain object is
        //       turned into one via SetDefaultGlobalBindings in the spec.
        global = vm.heap().allocate<GlobalObject>(realm);
    }

    // 14. If the host requires that the this binding in realm's global scope return an object other than the global object, then
    Object* this_value = nullptr;
    if (create_global_this_value) {
        // a. Let thisValue be such an object created in a host-defined manner.
        this_value = create_global_this_value(*realm);
    }
    // 15. Else,
    else {
        // a. Let thisValue be global.
        this_value = global;
    }

    // 16. Set realm.[[GlobalObject]] to global.
    realm->m_global_object = global;

    // 17. Set realm.[[GlobalEnv]] to NewGlobalEnvironment(global, thisValue).
    realm->m_global_environment = vm.heap().allocate<GlobalEnvironment>(*global, *this_value);

    // 18. Perform ? SetDefaultGlobalBindings(realm).
    set_default_global_bindings(*realm);

    // 19. Create any host-defined global object properties on global.
    global->initialize(*realm);

    // 20. Return unused.
    return new_context;
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
