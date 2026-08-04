/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/FederatedCredential.h>
#include <LibWeb/CredentialManagement/Credential.h>
#include <LibWeb/CredentialManagement/CredentialUserData.h>

namespace Web::CredentialManagement {

// https://www.w3.org/TR/credential-management-1/#dictdef-federatedcredentialrequestoptions
struct FederatedCredentialRequestOptions {
    Vector<String> providers;
    Vector<String> protocols;
};

// https://www.w3.org/TR/credential-management-1/#dictdef-federatedcredentialinit
using FederatedCredentialInit = Bindings::FederatedCredentialInit;

// https://w3c.github.io/webappsec-credential-management/#federatedcredential
class FederatedCredential final
    : public Credential
    , public CredentialUserData {
    WEB_WRAPPABLE(FederatedCredential, Credential);
    GC_DECLARE_ALLOCATOR(FederatedCredential);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<FederatedCredential>> create(FederatedCredentialInit const&);

    virtual ~FederatedCredential() override;

    Utf16String const& provider() const { return m_provider; }
    Optional<Utf16String> const& protocol() const { return m_protocol; }
    URL::Origin const& origin() const { return m_origin; }

    Utf16FlyString const& type() const override;

private:
    FederatedCredential(FederatedCredentialInit, URL::Origin);

    Utf16String m_provider;
    Optional<Utf16String> m_protocol;

    // https://www.w3.org/TR/credential-management-1/#dom-credential-origin-slot
    URL::Origin m_origin;
};

}
