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

static void reject_not_implemented_promise(GC::Ref<WebIDL::Promise> promise, StringView operation)
{
    auto& realm = WebIDL::promise_realm(promise);
    auto& vm = realm.vm();
    auto exception = vm.throw_completion<JS::InternalError>(JS::ErrorType::NotImplemented, operation);
    WebIDL::reject_promise_with_exception(promise, exception);
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-get
void CredentialsContainer::get(CredentialRequestOptions const&, GC::Ref<WebIDL::Promise> promise)
{
    dbgln("FIXME: Unimplemented CredentialsContainer::get()");
    reject_not_implemented_promise(promise, "get"sv);
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-store
void CredentialsContainer::store(Credential const&, GC::Ref<WebIDL::Promise> promise)
{
    dbgln("FIXME: Unimplemented CredentialsContainer::store()");
    reject_not_implemented_promise(promise, "store"sv);
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-create
void CredentialsContainer::create(CredentialCreationOptions const&, GC::Ref<WebIDL::Promise> promise)
{
    dbgln("FIXME: Unimplemented CredentialsContainer::create()");
    reject_not_implemented_promise(promise, "create"sv);
}

// https://www.w3.org/TR/credential-management-1/#dom-credentialscontainer-preventsilentaccess
void CredentialsContainer::prevent_silent_access(GC::Ref<WebIDL::Promise> promise)
{
    dbgln("FIXME: Unimplemented CredentialsContainer::prevent_silent_access()");
    reject_not_implemented_promise(promise, "prevent silent access"sv);
}

CredentialsContainer::CredentialsContainer()
{
}

}
