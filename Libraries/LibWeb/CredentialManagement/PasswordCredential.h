/*
 * Copyright (c) 2026, Altomani Gianluca <altomanigianluca@gmail.com>
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

class PasswordCredentialInterface final : public CredentialInterface {
    CREDENTIAL_INTERFACE(PasswordCredentialInterface);

public:
    virtual String type() const override { return "password"_string; }
    virtual String options_member_identifier() const override { return "password"_string; }
    virtual Optional<String> get_permission_policy() const override { return {}; }
    virtual Optional<String> create_permission_policy() const override { return {}; }

    virtual String discovery() const override { return "credential store"_string; }
    virtual bool supports_conditional_user_mediation() const override
    {
        // NOTE: PasswordCredential does not override is_conditional_mediation_available(),
        //       therefore conditional mediation is not supported.
        return false;
    }

    // https://w3c.github.io/webappsec-credential-management/#create-passwordcredential
    virtual WebIDL::ExceptionOr<Variant<Empty, GC::Ref<Credential>, GC::Ref<CreateCredentialAlgorithm>>> create(JS::Realm&, URL::Origin const&, CredentialCreationOptions const&, bool) const override;
};

// https://www.w3.org/TR/credential-management-1/#passwordcredential
class PasswordCredential final
    : public Credential
    , public CredentialUserData {
    WEB_PLATFORM_OBJECT(PasswordCredential, Credential);
    GC_DECLARE_ALLOCATOR(PasswordCredential);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> construct_impl(JS::Realm&, GC::Ref<HTML::HTMLFormElement>);
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> construct_impl(JS::Realm&, PasswordCredentialData const&);

    virtual ~PasswordCredential() override;

    String const& password() const { return m_password; }
    URL::Origin const& origin() const { return m_origin; }

    String type() const override { return "password"_string; }
    virtual CredentialInterface const& interface() const override
    {
        return PasswordCredentialInterface::the();
    }

private:
    PasswordCredential(JS::Realm&, PasswordCredentialData const&, URL::Origin);
    virtual void initialize(JS::Realm&) override;

    // TODO: Use Core::SecretString when it comes back
    String m_password;

    // https://www.w3.org/TR/credential-management-1/#dom-credential-origin-slot
    URL::Origin m_origin;
};

// https://www.w3.org/TR/credential-management-1/#dictdef-passwordcredentialdata
struct PasswordCredentialData : CredentialData {
    Optional<String> name;
    Optional<String> icon_url;
    String password;
};

// https://www.w3.org/TR/credential-management-1/#typedefdef-passwordcredentialinit
using PasswordCredentialInit = Variant<PasswordCredentialData, GC::Root<HTML::HTMLFormElement>>;

}
