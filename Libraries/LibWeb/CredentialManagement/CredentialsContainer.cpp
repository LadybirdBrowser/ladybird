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

JS::ThrowCompletionOr<GC::Ref<WebIDL::Promise>> CredentialsContainer::get(CredentialRequestOptions const&)
{
    return WebIDL::create_rejected_promise(realm(), JS::PrimitiveString::create(realm().vm(), "Not implemented"sv));
}

JS::ThrowCompletionOr<GC::Ref<WebIDL::Promise>> CredentialsContainer::store(Credential const&)
{
    return WebIDL::create_rejected_promise(realm(), JS::PrimitiveString::create(realm().vm(), "Not implemented"sv));
}

JS::ThrowCompletionOr<GC::Ref<WebIDL::Promise>> CredentialsContainer::create(CredentialCreationOptions const&)
{
    return WebIDL::create_rejected_promise(realm(), JS::PrimitiveString::create(realm().vm(), "Not implemented"sv));
}

JS::ThrowCompletionOr<GC::Ref<WebIDL::Promise>> CredentialsContainer::prevent_silent_access()
{
    return WebIDL::create_rejected_promise(realm(), JS::PrimitiveString::create(realm().vm(), "Not implemented"sv));
}

CredentialsContainer::CredentialsContainer(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void CredentialsContainer::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CredentialsContainer);
}

}
