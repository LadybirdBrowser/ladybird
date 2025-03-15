/*
 * Copyright (c) 2025, Bogi Napoleon Wennerstrøm <bogi.wennerstrom@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/LockPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::WebLocks {

class Lock final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Lock, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Lock);

public:
    static WebIDL::ExceptionOr<GC::Ref<Lock>> construct_impl(JS::Realm&, String const& name, Bindings::LockMode mode);

    virtual ~Lock() = default;

    String name() const;
    Bindings::LockMode mode() const;

private:
    Lock(JS::Realm&, String const& name, Bindings::LockMode mode);

    virtual void initialize(JS::Realm&) override;

    String m_name;
    Bindings::LockMode m_mode;
};

}
