/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CredentialManagement/Credential.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CredentialManagement {

// https://www.w3.org/TR/credential-management-1/#dom-credential-isconditionalmediationavailable
bool Credential::is_conditional_mediation_available()
{
    // 1. Return false.
    return false;
}

GC::Ref<WebIDL::Promise> Credential::is_conditional_mediation_available(JS::Realm& realm)
{
    return WebIDL::create_resolved_promise(realm, JS::Value(is_conditional_mediation_available()));
}

Credential::~Credential() { }

Credential::Credential()
{
}

Credential::Credential(String id)
    : m_id(move(id))
{
}

}
