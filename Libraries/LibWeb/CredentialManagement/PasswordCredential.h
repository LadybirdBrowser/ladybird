/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/Origin.h>
#include <LibWeb/Bindings/PasswordCredential.h>
#include <LibWeb/CredentialManagement/Credential.h>
#include <LibWeb/CredentialManagement/CredentialUserData.h>
#include <LibWeb/HTML/HTMLFormElement.h>

namespace Web::CredentialManagement {

// https://www.w3.org/TR/credential-management-1/#dictdef-passwordcredentialdata
using PasswordCredentialData = Bindings::PasswordCredentialData;

// https://www.w3.org/TR/credential-management-1/#passwordcredential
class PasswordCredential final
    : public Credential
    , public CredentialUserData {
    WEB_WRAPPABLE(PasswordCredential, Credential);
    GC_DECLARE_ALLOCATOR(PasswordCredential);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> create(URL::Origin, GC::Ref<HTML::HTMLFormElement>);
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> create(URL::Origin, PasswordCredentialData const&);
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> create_for_constructor(JS::Realm&, GC::Ref<HTML::HTMLFormElement>);
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> create_for_constructor(JS::Realm&, PasswordCredentialData const&);

    virtual ~PasswordCredential() override;

    String const& password() const { return m_password; }
    URL::Origin const& origin() const { return m_origin; }

    String type() const override { return "password"_string; }

private:
    PasswordCredential(PasswordCredentialData, URL::Origin);

    // TODO: Use Core::SecretString when it comes back
    String m_password;

    // https://www.w3.org/TR/credential-management-1/#dom-credential-origin-slot
    URL::Origin m_origin;
};

}
