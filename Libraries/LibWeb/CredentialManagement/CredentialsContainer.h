/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/CredentialsContainerPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CredentialManagement/Credential.h>
#include <LibWeb/CredentialManagement/FederatedCredential.h>
#include <LibWeb/CredentialManagement/PasswordCredential.h>
#include <LibWeb/DOM/AbortSignal.h>

namespace Web::CredentialManagement {

class CredentialsContainer final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CredentialsContainer, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CredentialsContainer);

public:
    [[nodiscard]] static GC::Ref<CredentialsContainer> create(JS::Realm&);

    virtual ~CredentialsContainer() override;

    GC::Ref<WebIDL::Promise> get(CredentialRequestOptions const& options);
    GC::Ref<WebIDL::Promise> store(Credential const& credential);
    GC::Ref<WebIDL::Promise> create(CredentialCreationOptions const& options);
    GC::Ref<WebIDL::Promise> prevent_silent_access();

private:
    explicit CredentialsContainer(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
};

struct CredentialRequestOptions {
    Bindings::CredentialMediationRequirement mediation { Bindings::CredentialMediationRequirement::Optional };
    GC::Ptr<DOM::AbortSignal> signal;

    Optional<bool> password;
    Optional<FederatedCredentialRequestOptions> federated;
};

struct CredentialCreationOptions {
    Bindings::CredentialMediationRequirement mediation { Bindings::CredentialMediationRequirement::Optional };
    GC::Ptr<DOM::AbortSignal> signal;

    Optional<PasswordCredentialInit> password;
    Optional<FederatedCredentialInit> federated;
};

}
