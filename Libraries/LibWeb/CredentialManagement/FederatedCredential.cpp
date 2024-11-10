/*
 * Copyright (c) 2024, Saksham Goyal <sakgoy2001@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/FederatedCredentialPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CredentialManagement/Credential.h>
#include <LibWeb/CredentialManagement/CredentialsContainer.h>
#include <LibWeb/CredentialManagement/FederatedCredential.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CredentialManagement {
JS_DEFINE_ALLOCATOR(FederatedCredential);

FederatedCredential::FederatedCredential(JS::Realm& realm)
    : Credential(realm)
{
}

void FederatedCredential::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(FederatedCredential);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<FederatedCredential>> FederatedCredential::construct_impl(JS::Realm& realm, FederatedCredentialInit& data)
{
    // 1. Let r be the result of executing Create a FederatedCredential
    //    from FederatedCredentialInit on data. If that threw an exception, rethrow that exception.
    auto r = realm.heap().allocate<FederatedCredential>(realm, realm, data);

    return r;
}

FederatedCredential::FederatedCredential(JS::Realm& realm, FederatedCredentialInit&)
    : Credential(realm)
{
    // auto r = realm.heap().allocate<FederatedCredential>(realm, realm, data);
}

}
