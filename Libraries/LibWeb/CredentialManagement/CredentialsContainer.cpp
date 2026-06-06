/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/CredentialManagement/CredentialsContainer.h>

namespace Web::CredentialManagement {

GC_DEFINE_ALLOCATOR(CredentialsContainer);

GC::Ref<CredentialsContainer> CredentialsContainer::create()
{
    return GC::Heap::the().allocate<CredentialsContainer>();
}

CredentialsContainer::~CredentialsContainer() { }

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-get
GC::Ref<WebIDL::Promise> CredentialsContainer::get(JS::Realm& realm, Bindings::CredentialRequestOptions const&)
{
    auto& vm = realm.vm();
    auto exception = vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "get"sv);
    return WebIDL::create_rejected_promise_from_exception(realm, exception);
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-store
GC::Ref<WebIDL::Promise> CredentialsContainer::store(JS::Realm& realm, Credential const&)
{
    auto& vm = realm.vm();
    auto exception = vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "store"sv);
    return WebIDL::create_rejected_promise_from_exception(realm, exception);
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-create
GC::Ref<WebIDL::Promise> CredentialsContainer::create(JS::Realm& realm, Bindings::CredentialCreationOptions const&)
{
    auto& vm = realm.vm();
    auto exception = vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "create"sv);
    return WebIDL::create_rejected_promise_from_exception(realm, exception);
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-preventsilentaccess
GC::Ref<WebIDL::Promise> CredentialsContainer::prevent_silent_access(JS::Realm& realm)
{
    auto& vm = realm.vm();
    auto exception = vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, "prevent silent access"sv);
    return WebIDL::create_rejected_promise_from_exception(realm, exception);
}

CredentialsContainer::CredentialsContainer()
{
}

}
