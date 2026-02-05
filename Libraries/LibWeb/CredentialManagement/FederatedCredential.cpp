/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CredentialManagement/FederatedCredential.h>
#include <LibWeb/CredentialManagement/FederatedCredentialOperations.h>

namespace Web::CredentialManagement {

GC_DEFINE_ALLOCATOR(FederatedCredential);

// https://www.w3.org/TR/credential-management-1/#dom-federatedcredential-federatedcredential
WebIDL::ExceptionOr<GC::Ref<FederatedCredential>> FederatedCredential::construct_impl(JS::Realm& realm, FederatedCredentialInit const& data)
{
    // 1. Let r be the result of executing Create a FederatedCredential from FederatedCredentialInit on data. If that
    // threw an exception, rethrow that exception.
    // 2. Return r.
    return create_federated_credential(realm, data);
}

FederatedCredential::~FederatedCredential()
{
}

FederatedCredential::FederatedCredential(JS::Realm& realm, FederatedCredentialInit const& init, URL::Origin origin)
    : Credential(realm, init.id)
    , CredentialUserData(init.name.value_or(String {}), init.icon_url.value_or(String {}))
    , m_provider(init.provider)
    , m_origin(move(origin))
{
}

void FederatedCredential::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(FederatedCredential);
    Base::initialize(realm);
}

}
