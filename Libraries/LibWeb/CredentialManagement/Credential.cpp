/*
 * Copyright (c) 2024, Saksham Goyal <sakgoy2001@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CredentialPrototype.h>
#include <LibWeb/Bindings/CredentialsContainerPrototype.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CredentialManagement/Credential.h>
#include <LibWeb/CredentialManagement/CredentialsContainer.h>
#include <LibWeb/CredentialManagement/FederatedCredential.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::CredentialManagement {
JS_DEFINE_ALLOCATOR(Credential);

Credential::Credential(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void Credential::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Credential);
}

JS::NonnullGCPtr<WebIDL::Promise> Credential::will_request_conditional_creation(JS::VM& vm)
{
    auto* realm = vm.current_realm();
    auto promise = WebIDL::create_promise(*realm);
    WebIDL::reject_promise(*realm, promise, WebIDL::UnknownError::create(*realm, "Function not completed"_string));
    return promise;
}

JS::NonnullGCPtr<WebIDL::Promise> Credential::is_conditional_mediation_available(JS::VM& vm)
{

    auto* realm = vm.current_realm();
    auto promise = WebIDL::create_promise(*realm);
    WebIDL::reject_promise(*realm, promise, WebIDL::UnknownError::create(*realm, "Function not completed"_string));
    return promise;
}

}
