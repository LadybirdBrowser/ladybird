/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CredentialManagement/PasswordCredential.h>
#include <LibWeb/CredentialManagement/PasswordCredentialOperations.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CredentialManagement {

GC_DEFINE_ALLOCATOR(PasswordCredential);

// https://www.w3.org/TR/credential-management-1/#dom-passwordcredential-passwordcredential
WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> PasswordCredential::create(URL::Origin origin, GC::Ref<HTML::HTMLFormElement> form)
{
    // 2. Let r be the result of executing Create a PasswordCredential from an HTMLFormElement given form and origin.
    // 3. If r is an exception, throw r. Otherwise, return r.
    return create_password_credential(form, move(origin));
}

// https://www.w3.org/TR/credential-management-1/#dom-passwordcredential-passwordcredential-data
WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> PasswordCredential::create(URL::Origin origin, PasswordCredentialData const& data)
{
    // 1. Let r be the result of executing Create a PasswordCredential from PasswordCredentialData on data.
    // 2. If r is an exception, throw r.
    return create_password_credential(data, move(origin));
}

WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> PasswordCredential::create_for_constructor(JS::Realm& realm, GC::Ref<HTML::HTMLFormElement> form)
{
    auto& window = HTML::relevant_window(realm.global_object());
    return create(HTML::relevant_settings_object(window).origin(), form);
}

WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> PasswordCredential::create_for_constructor(JS::Realm& realm, PasswordCredentialData const& data)
{
    auto& window = HTML::relevant_window(realm.global_object());
    return create(HTML::relevant_settings_object(window).origin(), data);
}

PasswordCredential::~PasswordCredential()
{
}

PasswordCredential::PasswordCredential(PasswordCredentialData data, URL::Origin origin)
    : Credential(move(data.id))
    , CredentialUserData(data.name.value_or(String {}), data.icon_url.value_or(String {}))
    , m_password(move(data.password))
    , m_origin(move(origin))
{
}

}
