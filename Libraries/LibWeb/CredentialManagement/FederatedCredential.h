/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/FederatedCredentialPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CredentialManagement/Credential.h>

namespace Web::CredentialManagement {

class FederatedCredentialInterface final : public CredentialInterface {
    CREDENTIAL_INTERFACE(FederatedCredentialInterface);

public:
    virtual String type() const override { return "federated"_string; }
    virtual String options_member_identifier() const override { return "federated"_string; }
    virtual Optional<String> get_permission_policy() const override { return {}; }
    virtual Optional<String> create_permission_policy() const override { return {}; }

    virtual String discovery() const override { return "credential store"_string; }
    virtual bool supports_conditional_user_mediation() const override
    {
        // NOTE: FederatedCredential does not override is_conditional_mediation_available(),
        //       therefore conditional mediation is not supported.
        return false;
    }
};

class FederatedCredential final : public Credential {
    WEB_PLATFORM_OBJECT(FederatedCredential, Credential);
    GC_DECLARE_ALLOCATOR(FederatedCredential);

public:
    [[nodiscard]] static GC::Ref<FederatedCredential> create(JS::Realm&);
    static WebIDL::ExceptionOr<GC::Ref<FederatedCredential>> construct_impl(JS::Realm&, FederatedCredentialInit const&);

    virtual ~FederatedCredential() override;

    String const& provider() const { return m_provider; }
    Optional<String> const& protocol() const { return m_protocol; }

    String type() const override { return "federated"_string; }
    virtual CredentialInterface const* interface() const override
    {
        return FederatedCredentialInterface::the();
    }

private:
    explicit FederatedCredential(JS::Realm&);
    virtual void initialize(JS::Realm&) override;

    String m_provider;
    Optional<String> m_protocol;
};

struct FederatedCredentialRequestOptions {
    Optional<Vector<String>> providers;
    Optional<Vector<String>> protocols;
};

struct FederatedCredentialInit : CredentialData {
    Optional<String> name;
    Optional<String> icon_url;
    String origin;
    String provider;
    Optional<String> protocol;
};

}
