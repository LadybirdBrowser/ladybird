/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CredentialManagement/PasswordCredential.h>

namespace Web::CredentialManagement {

GC_DEFINE_ALLOCATOR(PasswordCredential);

GC::Ref<PasswordCredential> PasswordCredential::create(JS::Realm& realm)
{
    return realm.create<PasswordCredential>(realm);
}

WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> PasswordCredential::construct_impl(JS::Realm& realm, HTML::HTMLFormElement const&)
{
    return JS::throw_completion(JS::PrimitiveString::create(realm.vm(), "Not implemented"sv));
}

WebIDL::ExceptionOr<GC::Ref<PasswordCredential>> PasswordCredential::construct_impl(JS::Realm& realm, PasswordCredentialData const&)
{
    return JS::throw_completion(JS::PrimitiveString::create(realm.vm(), "Not implemented"sv));
}

PasswordCredential::~PasswordCredential()
{
}

PasswordCredential::PasswordCredential(JS::Realm& realm)
    : Credential(realm)
{
}

void PasswordCredential::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PasswordCredential);
}

}
