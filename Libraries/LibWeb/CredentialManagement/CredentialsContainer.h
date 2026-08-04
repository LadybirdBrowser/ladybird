/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CredentialsContainer.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CredentialManagement/Credential.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CredentialManagement {

using CredentialRequestOptions = Bindings::CredentialRequestOptions;
using CredentialCreationOptions = Bindings::CredentialCreationOptions;

class CredentialsContainer final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(CredentialsContainer, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(CredentialsContainer);

public:
    [[nodiscard]] static GC::Ref<CredentialsContainer> create();

    virtual ~CredentialsContainer() override;

    void get(CredentialRequestOptions const&, GC::Ref<WebIDL::Promise>);
    void store(Credential const& credential, GC::Ref<WebIDL::Promise>);
    void create(CredentialCreationOptions const&, GC::Ref<WebIDL::Promise>);
    void prevent_silent_access(GC::Ref<WebIDL::Promise>);

private:
    CredentialsContainer();
};

}
