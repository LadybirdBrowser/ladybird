/*
 * Copyright (c) 2024, Saksham Goyal <sakgoy2001@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CredentialsContainerPrototype.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CredentialManagement/Credential.h>
#include <LibWeb/CredentialManagement/CredentialsContainer.h>
#include <LibWeb/CredentialManagement/FederatedCredential.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::CredentialManagement {
JS_DEFINE_ALLOCATOR(CredentialsContainer);

CredentialsContainer::CredentialsContainer(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

void CredentialsContainer::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CredentialsContainer);
}

JS::NonnullGCPtr<WebIDL::Promise> CredentialsContainer::get(CredentialRequestOptions&)
{
    auto& realm = this->realm();
    auto promise = WebIDL::create_promise(realm);
    WebIDL::reject_promise(realm, promise, WebIDL::UnknownError::create(realm, "Function not completed"_string));
    return promise;
}
JS::NonnullGCPtr<WebIDL::Promise> CredentialsContainer::store(Credential&)
{
    auto& realm = this->realm();
    auto promise = WebIDL::create_promise(realm);
    WebIDL::reject_promise(realm, promise, WebIDL::UnknownError::create(realm, "Function not completed"_string));
    return promise;
}
JS::NonnullGCPtr<WebIDL::Promise> CredentialsContainer::create(CredentialCreationOptions&)
{
    auto& realm = this->realm();
    auto promise = WebIDL::create_promise(realm);
    WebIDL::reject_promise(realm, promise, WebIDL::UnknownError::create(realm, "Function not completed"_string));
    return promise;
}
JS::NonnullGCPtr<WebIDL::Promise> CredentialsContainer::prevent_silent_access()
{
    auto& realm = this->realm();
    auto promise = WebIDL::create_promise(realm);
    WebIDL::reject_promise(realm, promise, WebIDL::UnknownError::create(realm, "Function not completed"_string));
    WebIDL::resolve_promise(realm, promise, JS::js_undefined());
    return promise;
}

}
