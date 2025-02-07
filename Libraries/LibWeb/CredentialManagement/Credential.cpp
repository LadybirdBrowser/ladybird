/*
 * Copyright (c) 2026, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CredentialManagement/Credential.h>

namespace Web::CredentialManagement {

WebIDL::ExceptionOr<Variant<Empty, GC::Ref<Credential>, GC::Ref<CreateCredentialAlgorithm>>> CredentialInterface::create(JS::Realm&, URL::Origin const&, CredentialCreationOptions const&, bool) const
{
    // 1. Return null.
    return Empty {};
}

WebIDL::ExceptionOr<void> CredentialInterface::store(JS::Realm& realm, bool) const
{
    // 1. Throw a NotSupportedError.
    return JS::throw_completion(WebIDL::NotSupportedError::create(realm, "store"_utf16));
}

WebIDL::ExceptionOr<Variant<Empty, GC::Ref<Credential>>> CredentialInterface::discover_from_external_source(JS::Realm&, URL::Origin const&, CredentialRequestOptions const&, bool) const
{
    // 1. Return null.
    return Empty {};
}

WebIDL::ExceptionOr<Vector<Credential>> CredentialInterface::collect_from_credential_store(JS::Realm&, URL::Origin const&, CredentialRequestOptions const&, bool) const
{
    // 1. Return an empty set.
    return Vector<Credential> {};
}

// https://www.w3.org/TR/credential-management-1/#dom-credential-isconditionalmediationavailable
GC::Ref<WebIDL::Promise> Credential::is_conditional_mediation_available(JS::VM& vm)
{
    // 1. Return a promise resolved with false.
    return WebIDL::create_resolved_promise(*vm.current_realm(), JS::Value(false));
}

Credential::~Credential() { }

Credential::Credential(JS::Realm& realm)
    : PlatformObject(realm)
{
}

Credential::Credential(JS::Realm& realm, String id)
    : PlatformObject(realm)
    , m_id(move(id))
{
}

void Credential::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Credential);
    Base::initialize(realm);
}

}
