/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Credential.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CredentialManagement {

// https://www.w3.org/TR/credential-management-1/#credential
class Credential : public Bindings::Wrappable {
    WEB_WRAPPABLE(Credential, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Credential);

public:
    static GC::Ref<WebIDL::Promise> is_conditional_mediation_available(JS::VM&);

    virtual ~Credential() override;

    String const& id() const { return m_id; }

    virtual String type() const = 0;

protected:
    explicit Credential(JS::Realm&);
    Credential(JS::Realm&, String id);

    String m_id;
};

// https://www.w3.org/TR/credential-management-1/#dictdef-credentialdata
struct CredentialData {
    String id;
};

}
