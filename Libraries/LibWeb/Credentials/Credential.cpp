/*
 * Copyright (c) 2024, Miguel Sacristán <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Credential.h"

#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/CredentialPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Credentials {
GC_DEFINE_ALLOCATOR(Credential);

WebIDL::ExceptionOr<GC::Ref<Credential>> Credential::construct_impl(JS::Realm& realm)
{
    return realm.create<Credential>(realm);
}

// https://www.w3.org/TR/credential-management-1/#dom-credential-isconditionalmediationavailable
GC::Ref<WebIDL::Promise>
Credential::is_conditional_mediation_available(JS::VM& vm)
{
    auto& realm = *vm.current_realm();
    auto const promise = WebIDL::create_promise(realm);

    WebIDL::resolve_promise(realm, promise, JS::Value(false));

    return promise;
}

// https://www.w3.org/TR/credential-management-1/#dom-credential-willrequestconditionalcreation
GC::Ref<WebIDL::Promise> Credential::will_request_conditional_creation(JS::VM& vm)
{
    auto& realm = *vm.current_realm();
    auto const promise = WebIDL::create_promise(realm);

    WebIDL::resolve_promise(realm, promise, JS::js_undefined());

    return promise;
}

void Credential::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Credential);
}

}
