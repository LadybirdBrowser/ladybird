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
#include <LibWeb/CredentialManagement/CredentialUserData.h>

namespace Web::CredentialManagement {

// https://w3c.github.io/webappsec-credential-management/#federatedcredential
class FederatedCredential final
    : public Credential
    , public CredentialUserData {
    WEB_PLATFORM_OBJECT(FederatedCredential, Credential);
    GC_DECLARE_ALLOCATOR(FederatedCredential);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<FederatedCredential>> construct_impl(JS::Realm&, FederatedCredentialInit const&);

    virtual ~FederatedCredential() override;

    String const& provider() const { return m_provider; }
    Optional<String> const& protocol() const { return m_protocol; }
    URL::Origin const& origin() const { return m_origin; }

    String type() const override { return "federated"_string; }

private:
    FederatedCredential(JS::Realm&, FederatedCredentialInit const&, URL::Origin);
    virtual void initialize(JS::Realm&) override;

    String m_provider;
    Optional<String> m_protocol;

    // https://www.w3.org/TR/credential-management-1/#dom-credential-origin-slot
    URL::Origin m_origin;
};

// https://www.w3.org/TR/credential-management-1/#dictdef-federatedcredentialrequestoptions
struct FederatedCredentialRequestOptions {
    Optional<Vector<String>> providers;
    Optional<Vector<String>> protocols;
};

// https://www.w3.org/TR/credential-management-1/#dictdef-federatedcredentialinit
struct FederatedCredentialInit : CredentialData {
    Optional<String> name;
    Optional<String> icon_url;
    String provider;
    Optional<String> protocol;
};

}
