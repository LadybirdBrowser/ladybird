/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/CredentialsContainer.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CredentialManagement/Credential.h>
#include <LibWeb/CredentialManagement/FederatedCredential.h>
#include <LibWeb/CredentialManagement/PasswordCredential.h>
#include <LibWeb/DOM/AbortSignal.h>

namespace Web::CredentialManagement {

class CredentialsContainer final : public Bindings::Wrappable {
    WEB_WRAPPABLE(CredentialsContainer, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(CredentialsContainer);

public:
    [[nodiscard]] static GC::Ref<CredentialsContainer> create();

    virtual ~CredentialsContainer() override;

    GC::Ref<WebIDL::Promise> get(JS::Realm&, Bindings::CredentialRequestOptions const& options);
    GC::Ref<WebIDL::Promise> store(JS::Realm&, Credential const& credential);
    GC::Ref<WebIDL::Promise> create(JS::Realm&, Bindings::CredentialCreationOptions const& options);
    GC::Ref<WebIDL::Promise> prevent_silent_access(JS::Realm&);

private:
    CredentialsContainer();
};

}
