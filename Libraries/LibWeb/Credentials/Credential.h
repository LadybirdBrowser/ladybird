/*
 * Copyright (c) 2024, Miguel Sacristán <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Credentials {

class Credential : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Credential, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Credential);

public:
    virtual ~Credential() override = default;

    static WebIDL::ExceptionOr<GC::Ref<Credential>> construct_impl(JS::Realm&);

    virtual String id() const { return String {}; }
    String type() const { return m_type; }

    static GC::Ref<WebIDL::Promise> is_conditional_mediation_available(JS::VM&);
    static GC::Ref<WebIDL::Promise> will_request_conditional_creation(JS::VM&);

protected:
    explicit Credential(JS::Realm& realm)
        : PlatformObject(realm)
    {
    }
    explicit Credential(JS::Realm& realm, String const& type)
        : PlatformObject(realm)
        , m_type(type)
    {
    }

    virtual void initialize(JS::Realm& realm) override;

    String m_type {};
};

}
