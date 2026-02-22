/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CredentialManagement/PasswordCredential.h>
#include <LibWeb/CredentialManagement/PasswordCredentialOperations.h>

namespace Web::CredentialManagement {

GC_DEFINE_ALLOCATOR(PasswordCredential);

// https://www.w3.org/TR/credential-management-1/#dom-passwordcredential-passwordcredential
WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> PasswordCredential::construct_impl(JS::Realm& realm, GC::Ref<HTML::HTMLFormElement> form)
{
    // 1. Let origin be the current settings object's origin.
    auto origin = HTML::current_principal_settings_object().origin();

    // 2. Let r be the result of executing Create a PasswordCredential from an HTMLFormElement given form and origin.
    // 3. If r is an exception, throw r. Otherwise, return r.
    return create_password_credential(realm, form, move(origin));
}

// https://www.w3.org/TR/credential-management-1/#dom-passwordcredential-passwordcredential-data
WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> PasswordCredential::construct_impl(JS::Realm& realm, PasswordCredentialData const& data)
{
    // AD-HOC: Let origin be the current settings object's origin.
    auto origin = HTML::current_principal_settings_object().origin();

    // 1. Let r be the result of executing Create a PasswordCredential from PasswordCredentialData on data.
    // 2. If r is an exception, throw r.
    return create_password_credential(realm, data, move(origin));
}

PasswordCredential::~PasswordCredential()
{
}

PasswordCredential::PasswordCredential(JS::Realm& realm, PasswordCredentialData const& data, URL::Origin origin)
    : Credential(realm, data.id)
    , CredentialUserData(data.name.value_or(String {}), data.icon_url.value_or(String {}))
    , m_password(data.password)
    , m_origin(move(origin))
{
}

void PasswordCredential::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PasswordCredential);
    Base::initialize(realm);
}

}
