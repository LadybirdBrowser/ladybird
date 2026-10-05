/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

using CreateRealmObject = Function<GC::Ref<Object>(Realm&)>;

static JSObject* create_realm_object(void* context, JSRealm* realm)
{
    auto& create_object = *static_cast<CreateRealmObject*>(context);
    return object_to_abi(*create_object(*cell_from_abi<Realm>(realm)));
}

// 9.3.3 InitializeHostDefinedRealm ( ), https://tc39.es/ecma262/#sec-initializehostdefinedrealm
ThrowCompletionOr<NonnullOwnPtr<ExecutionContext>> Realm::initialize_host_defined_realm(VM& vm, CreateRealmObject create_global_object, CreateRealmObject create_global_this_value)
{
    // The runtime constructs the realm's execution context, which it pushes onto the execution context stack, in the
    // context this creates.
    auto new_context = ExecutionContext::create(0, ReadonlySpan<Value> {}, 0);
    TRY(completion_from_abi<void>(js_realm_initialize_host_defined_realm(
        vm_to_abi(vm),
        new_context.ptr(),
        create_global_object ? create_realm_object : nullptr,
        &create_global_object,
        create_global_this_value ? create_realm_object : nullptr,
        &create_global_this_value)));
    return new_context;
}

void Realm::set_host_defined(GC::Ptr<GC::Cell> host_defined)
{
    js_realm_set_host_defined(cell_to_abi<JSRealm>(*this), host_defined.ptr());
}

}
