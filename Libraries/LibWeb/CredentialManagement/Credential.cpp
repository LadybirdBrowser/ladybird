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
    auto* realm = vm.current_realm();
    return WebIDL::create_rejected_promise_from_exception(*realm, vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "is conditional mediation available"sv));
}

// https://www.w3.org/TR/credential-management-1/#dom-credential-willrequestconditionalcreation
GC::Ref<WebIDL::Promise> Credential::will_request_conditional_creation(JS::VM& vm)
{
    auto* realm = vm.current_realm();
    return WebIDL::create_rejected_promise_from_exception(*realm, vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "will request conditional creation"sv));
}

Credential::~Credential() { }

Credential::Credential(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void Credential::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Credential);
}
}
