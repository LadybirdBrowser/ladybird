/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CredentialManagement/Credential.h>

namespace Web::CredentialManagement {

// https://www.w3.org/TR/credential-management-1/#dom-credential-isconditionalmediationavailable
GC::Ref<WebIDL::Promise> Credential::is_conditional_mediation_available(JS::Realm& realm)
{
    // 1. Return a promise resolved with false.
    return WebIDL::create_resolved_promise(realm, JS::Value(false));
}

Credential::~Credential() { }

Credential::Credential()
    : Bindings::Wrappable()
{
}

Credential::Credential(String id)
    : Bindings::Wrappable()
    , m_id(move(id))
{
}

}
