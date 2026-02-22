/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/CredentialPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CredentialManagement {

// https://www.w3.org/TR/credential-management-1/#credential
class Credential : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Credential, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Credential);

public:
    static GC::Ref<WebIDL::Promise> is_conditional_mediation_available(JS::VM&);

    virtual ~Credential() override;

    String const& id() const { return m_id; }

    virtual String type() const = 0;

protected:
    explicit Credential(JS::Realm&);
    Credential(JS::Realm&, String id);
    virtual void initialize(JS::Realm&) override;

    String m_id;
};

// https://www.w3.org/TR/credential-management-1/#dictdef-credentialdata
struct CredentialData {
    String id;
};

}
