/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Error.h>
#include <LibWeb/CredentialManagement/CredentialsContainer.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CredentialManagement {

GC_DEFINE_ALLOCATOR(CredentialsContainer);

GC::Ref<CredentialsContainer> CredentialsContainer::create()
{
    return GC::Heap::the().allocate<CredentialsContainer>();
}

CredentialsContainer::~CredentialsContainer() { }

static void reject_not_implemented_promise(JS::Realm& realm, GC::Ref<WebIDL::Promise> promise, StringView operation)
{
    auto& vm = realm.vm();
    auto exception = vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, operation);
    WebIDL::reject_promise_with_exception(realm, promise, exception);
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-get
void CredentialsContainer::get(JS::Realm& realm, CredentialRequestOptions const&, GC::Ref<WebIDL::Promise> promise)
{
    dbgln("FIXME: Unimplemented CredentialsContainer::get()");
    reject_not_implemented_promise(realm, promise, "get"sv);
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-store
void CredentialsContainer::store(JS::Realm& realm, Credential const&, GC::Ref<WebIDL::Promise> promise)
{
    dbgln("FIXME: Unimplemented CredentialsContainer::store()");
    reject_not_implemented_promise(realm, promise, "store"sv);
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-create
void CredentialsContainer::create(JS::Realm& realm, CredentialCreationOptions const&, GC::Ref<WebIDL::Promise> promise)
{
    dbgln("FIXME: Unimplemented CredentialsContainer::create()");
    reject_not_implemented_promise(realm, promise, "create"sv);
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-preventsilentaccess
void CredentialsContainer::prevent_silent_access(JS::Realm& realm, GC::Ref<WebIDL::Promise> promise)
{
    dbgln("FIXME: Unimplemented CredentialsContainer::prevent_silent_access()");
    reject_not_implemented_promise(realm, promise, "prevent silent access"sv);
}

CredentialsContainer::CredentialsContainer()
{
}

}
