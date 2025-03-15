/*
 * Copyright (c) 2025, Bogi Napoleon Wennerstrøm <bogi.wennerstrom@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/LockManagerPrototype.h>
#include <LibWeb/Bindings/LockPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/AbortSignal.h>
#include <jmorecfg.h>

namespace Web::WebLocks {

// https://w3c.github.io/web-locks/#dictdef-lockoptions
struct LockOptions {
    Bindings::LockMode mode { Bindings::LockMode::Exclusive };
    boolean if_available { false };
    boolean steal { false };
    GC::Ptr<DOM::AbortSignal> signal;
};

// https://w3c.github.io/web-locks/#lockmanager
class LockManager final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(LockManager, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(LockManager);

public:
    static WebIDL::ExceptionOr<GC::Ref<LockManager>> construct_impl(JS::Realm&);

    virtual ~LockManager() = default;

    GC::Ref<WebIDL::Promise> request(String name, GC::Ref<WebIDL::CallbackType> callback) const;
    GC::Ref<WebIDL::Promise> request(String name, LockOptions options, GC::Ref<WebIDL::CallbackType> callback) const;

private:
    LockManager(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
