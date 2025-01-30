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

class Credential : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Credential, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Credential);

public:
    [[nodiscard]] static GC::Ref<Credential> create(JS::Realm&);

    static GC::Ref<WebIDL::Promise> is_conditional_mediation_available(JS::VM&);
    static GC::Ref<WebIDL::Promise> will_request_conditional_creation(JS::VM&);

    virtual ~Credential() override;

    String const& id() { return m_id; }
    String const& name() { return m_name; }
    String const& icon_url() { return m_icon_url; }

    virtual String type() = 0;

protected:
    explicit Credential(JS::Realm&);
    virtual void initialize(JS::Realm&) override;

    String m_id;
    String m_name;
    String m_icon_url;
};

struct CredentialData {
    String id;
};

}
