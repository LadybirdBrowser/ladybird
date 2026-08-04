/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::CredentialManagement {

// https://www.w3.org/TR/credential-management-1/#credential
class Credential : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Credential, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Credential);

public:
    static bool is_conditional_mediation_available();
    static GC::Ref<WebIDL::Promise> is_conditional_mediation_available_impl(JS::VM&);

    virtual ~Credential() override;

    Utf16String const& id() const { return m_id; }

    virtual Utf16FlyString const& type() const = 0;

protected:
    explicit Credential();
    Credential(Utf16String id);

    Utf16String m_id;
};

// https://www.w3.org/TR/credential-management-1/#dictdef-credentialdata
struct CredentialData {
    Utf16String id;
};

}
