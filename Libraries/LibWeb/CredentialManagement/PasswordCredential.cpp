/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CredentialManagement/PasswordCredential.h>
#include <LibWeb/CredentialManagement/PasswordCredentialOperations.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::CredentialManagement {

GC_DEFINE_ALLOCATOR(PasswordCredential);

// https://www.w3.org/TR/credential-management-1/#dom-passwordcredential-passwordcredential
WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> PasswordCredential::construct_impl(HTML::Window& window, GC::Ref<HTML::HTMLFormElement> form)
{
    // 1. Let origin be the current settings object's origin.
    auto origin = HTML::relevant_settings_object(window).origin();

    // 2. Let r be the result of executing Create a PasswordCredential from an HTMLFormElement given form and origin.
    // 3. If r is an exception, throw r. Otherwise, return r.
    return create_password_credential(form, move(origin));
}

// https://www.w3.org/TR/credential-management-1/#dom-passwordcredential-passwordcredential-data
WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> PasswordCredential::construct_impl(HTML::Window& window, Bindings::PasswordCredentialData const& data)
{
    // AD-HOC: Let origin be the current settings object's origin.
    auto origin = HTML::relevant_settings_object(window).origin();

    // 1. Let r be the result of executing Create a PasswordCredential from PasswordCredentialData on data.
    // 2. If r is an exception, throw r.
    return create_password_credential(data, move(origin));
}

PasswordCredential::~PasswordCredential()
{
}

PasswordCredential::PasswordCredential(Bindings::PasswordCredentialData const& data, URL::Origin origin)
    : Credential(data.id)
    , CredentialUserData(data.name.value_or(String {}), data.icon_url.value_or(String {}))
    , m_password(data.password)
    , m_origin(move(origin))
{
}

}
