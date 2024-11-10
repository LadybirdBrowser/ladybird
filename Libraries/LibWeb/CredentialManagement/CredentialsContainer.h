/*
 * Copyright (c) 2024, Saksham Goyal <sakgoy2001@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/AbortController.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CredentialManagement {

class CredentialsContainer final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CredentialsContainer, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(CredentialsContainer);

public:
    virtual ~CredentialsContainer() override = default;

    JS::NonnullGCPtr<WebIDL::Promise> get(CredentialRequestOptions& options);
    JS::NonnullGCPtr<WebIDL::Promise> store(Credential& cred);
    JS::NonnullGCPtr<WebIDL::Promise> create(CredentialCreationOptions& options);
    JS::NonnullGCPtr<WebIDL::Promise> prevent_silent_access();

private:
    CredentialsContainer(JS::Realm& realm);

    virtual void initialize(JS::Realm& realm) override;
};
}
