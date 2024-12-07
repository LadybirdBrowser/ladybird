/*
 * Copyright (c) 2024, Miguel Sacristán <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIDL/Types.h>
#include <LibWeb/Bindings/CredentialsContainerPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Credentials {

class CredentialsContainer final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CredentialsContainer, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CredentialsContainer);

public:
    static WebIDL::ExceptionOr<GC::Ref<CredentialsContainer>> construct_impl(JS::Realm&);

    [[nodiscard]] GC::Ref<WebIDL::Promise> get(CredentialRequestOptions const&);

private:
    explicit CredentialsContainer(JS::Realm& realm);

    void initialize(JS::Realm& realm) override;
};

struct CredentialRequestOptions {
    Bindings::CredentialMediationRequirement mediation = Bindings::CredentialMediationRequirement::Optional;
    GC::Ptr<DOM::AbortSignal> signal;
};

struct CredentialCreationOptions {
    Bindings::CredentialMediationRequirement mediation = Bindings::CredentialMediationRequirement::Optional;
    GC::Ptr<DOM::AbortSignal> signal;
};

// https://www.w3.org/TR/credential-management-1/#credentialrequestoptions-dictionary
Vector<IDL::Interface> relevant_credential_interface_objects(Variant<CredentialCreationOptions, CredentialRequestOptions> options);
}
