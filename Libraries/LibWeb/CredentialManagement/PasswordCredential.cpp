/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CredentialManagement/PasswordCredential.h>

namespace Web::CredentialManagement {

GC_DEFINE_ALLOCATOR(PasswordCredential);

GC::Ref<PasswordCredential> PasswordCredential::create(JS::Realm& realm)
{
    return realm.create<PasswordCredential>(realm);
}

// https://www.w3.org/TR/credential-management-1/#dom-passwordcredential-passwordcredential
WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> PasswordCredential::construct_impl(JS::Realm& realm, HTML::HTMLFormElement const&)
{
    return realm.vm().throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "construct"sv);
}

// https://www.w3.org/TR/credential-management-1/#dom-passwordcredential-passwordcredential-data
WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> PasswordCredential::construct_impl(JS::Realm& realm, PasswordCredentialData const&)
{
    return realm.vm().throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "construct"sv);
}

PasswordCredential::~PasswordCredential()
{
}

PasswordCredential::PasswordCredential(JS::Realm& realm)
    : Credential(realm)
{
}

void PasswordCredential::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PasswordCredential);
}

}
