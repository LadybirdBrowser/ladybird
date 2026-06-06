/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CredentialManagement/CredentialsContainer.h>

namespace Web::CredentialManagement {

GC_DEFINE_ALLOCATOR(CredentialsContainer);

GC::Ref<CredentialsContainer> CredentialsContainer::create(JS::Realm& realm)
{
    return realm.create<CredentialsContainer>(realm);
}

CredentialsContainer::~CredentialsContainer() { }

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-get
GC::Ref<WebIDL::Promise> CredentialsContainer::get(Bindings::CredentialRequestOptions const&)
{
    auto& vm = this->realm().vm();
    auto* realm = vm.current_realm();
    auto exception = vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "get"sv);
    return WebIDL::create_rejected_promise_from_exception(*realm, exception);
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-store
GC::Ref<WebIDL::Promise> CredentialsContainer::store(Credential const&)
{
    auto& vm = this->realm().vm();
    auto* realm = vm.current_realm();
    auto exception = vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "store"sv);
    return WebIDL::create_rejected_promise_from_exception(*realm, exception);
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-create
GC::Ref<WebIDL::Promise> CredentialsContainer::create(Bindings::CredentialCreationOptions const&)
{
    auto& vm = this->realm().vm();
    auto* realm = vm.current_realm();
    auto exception = vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "create"sv);
    return WebIDL::create_rejected_promise_from_exception(*realm, exception);
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-preventsilentaccess
GC::Ref<WebIDL::Promise> CredentialsContainer::prevent_silent_access()
{
    auto& vm = this->realm().vm();
    auto* realm = vm.current_realm();
    auto exception = vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "prevent silent access"sv);
    return WebIDL::create_rejected_promise_from_exception(*realm, exception);
}

CredentialsContainer::CredentialsContainer(JS::Realm& realm)
    : Wrappable(realm)
{
}

}
