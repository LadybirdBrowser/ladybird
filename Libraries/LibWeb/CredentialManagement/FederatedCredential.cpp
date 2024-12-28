/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CredentialManagement/FederatedCredential.h>

namespace Web::CredentialManagement {

GC_DEFINE_ALLOCATOR(FederatedCredential);

GC::Ref<FederatedCredential> FederatedCredential::create(JS::Realm& realm)
{
    return realm.create<FederatedCredential>(realm);
}

WebIDL::ExceptionOr<GC::Ref<FederatedCredential>> FederatedCredential::construct_impl(JS::Realm& realm, FederatedCredentialInit const&)
{
    return JS::throw_completion(JS::PrimitiveString::create(realm.vm(), "Not implemented"sv));
}

FederatedCredential::~FederatedCredential()
{
}

FederatedCredential::FederatedCredential(JS::Realm& realm)
    : Credential(realm)
{
}

void FederatedCredential::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(FederatedCredential);
}

}
