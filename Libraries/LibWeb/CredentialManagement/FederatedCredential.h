/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/FederatedCredential.h>
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
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<FederatedCredential>> construct_impl(JS::Realm&, Bindings::FederatedCredentialInit const&);

    virtual ~FederatedCredential() override;

    Utf16String const& provider() const { return m_provider; }
    Optional<Utf16String> const& protocol() const { return m_protocol; }
    URL::Origin const& origin() const { return m_origin; }

    Utf16FlyString const& type() const override;

private:
    FederatedCredential(JS::Realm&, Bindings::FederatedCredentialInit const&, URL::Origin);
    virtual void initialize(JS::Realm&) override;

    Utf16String m_provider;
    Optional<Utf16String> m_protocol;

    // https://www.w3.org/TR/credential-management-1/#dom-credential-origin-slot
    URL::Origin m_origin;
};

// https://www.w3.org/TR/credential-management-1/#dictdef-federatedcredentialrequestoptions
struct FederatedCredentialRequestOptions {
    Optional<Vector<Utf16String>> providers;
    Optional<Vector<Utf16String>> protocols;
};

// https://www.w3.org/TR/credential-management-1/#dictdef-federatedcredentialinit
struct FederatedCredentialInit : CredentialData {
    Optional<Utf16String> name;
    Optional<Utf16String> icon_url;
    Utf16String provider;
    Optional<Utf16String> protocol;
};

}
