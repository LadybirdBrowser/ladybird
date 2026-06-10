/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/CredentialsContainer.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CredentialManagement/Credential.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CredentialManagement {

using CredentialRequestOptions = Bindings::CredentialRequestOptions;
using CredentialCreationOptions = Bindings::CredentialCreationOptions;

class CredentialsContainer final : public Bindings::Wrappable {
    WEB_WRAPPABLE(CredentialsContainer, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(CredentialsContainer);

public:
    [[nodiscard]] static GC::Ref<CredentialsContainer> create();

    virtual ~CredentialsContainer() override;

    void get(JS::Realm&, CredentialRequestOptions const&, GC::Ref<WebIDL::Promise>);
    void store(JS::Realm&, Credential const& credential, GC::Ref<WebIDL::Promise>);
    void create(JS::Realm&, CredentialCreationOptions const&, GC::Ref<WebIDL::Promise>);
    void prevent_silent_access(JS::Realm&, GC::Ref<WebIDL::Promise>);

private:
    CredentialsContainer();
};

}
