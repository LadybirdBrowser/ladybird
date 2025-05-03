/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CredentialManagement/Credential.h>

namespace Web::CredentialManagement {

// https://www.w3.org/TR/credential-management-1/#dom-credential-isconditionalmediationavailable
GC::Ref<WebIDL::Promise> Credential::is_conditional_mediation_available(JS::VM& vm)
{
    // 1. Return a promise resolved with false.
    return WebIDL::create_resolved_promise(*vm.current_realm(), JS::Value(false));
}

// https://www.w3.org/TR/credential-management-1/#dom-credential-willrequestconditionalcreation
GC::Ref<WebIDL::Promise> Credential::will_request_conditional_creation(JS::VM& vm)
{
    // 1. Return a promise resolved with undefined.
    return WebIDL::create_resolved_promise(*vm.current_realm(), JS::js_undefined());
}

Credential::~Credential() { }

Credential::Credential(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void Credential::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Credential);
    Base::initialize(realm);
}
}
