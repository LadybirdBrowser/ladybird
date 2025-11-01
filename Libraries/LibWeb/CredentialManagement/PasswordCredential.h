/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PasswordCredentialPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CredentialManagement/Credential.h>
#include <LibWeb/CredentialManagement/CredentialUserData.h>
#include <LibWeb/HTML/HTMLFormElement.h>

namespace Web::CredentialManagement {

class PasswordCredential final
    : public Credential
    , public CredentialUserData {
    WEB_PLATFORM_OBJECT(PasswordCredential, Credential);
    GC_DECLARE_ALLOCATOR(PasswordCredential);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> construct_impl(JS::Realm&, GC::Ptr<HTML::HTMLFormElement> const&);
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> construct_impl(JS::Realm&, PasswordCredentialData const&);

    virtual ~PasswordCredential() override;

    String const& password() { return m_password; }
    String const& origin() { return m_origin; }

    String type() override { return "password"_string; }

private:
    PasswordCredential(JS::Realm&, PasswordCredentialData const&);
    virtual void initialize(JS::Realm&) override;

    // TODO: Use Core::SecretString when it comes back
    String m_password;

    // https://www.w3.org/TR/credential-management-1/#dom-credential-origin-slot
    String m_origin;
};

struct PasswordCredentialData : CredentialData {
    Optional<String> name;
    Optional<String> icon_url;
    String origin;
    String password;
};

using PasswordCredentialInit = Variant<PasswordCredentialData, GC::Root<HTML::HTMLFormElement>>;

}
