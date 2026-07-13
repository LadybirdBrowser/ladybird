/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Credential.h>
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

    Utf16String const& id() const { return m_id; }

    virtual Utf16FlyString const& type() const = 0;

protected:
    explicit Credential(JS::Realm&);
    Credential(JS::Realm&, Utf16String id);
    virtual void initialize(JS::Realm&) override;

    Utf16String m_id;
};

// https://www.w3.org/TR/credential-management-1/#dictdef-credentialdata
struct CredentialData {
    Utf16String id;
};

}
