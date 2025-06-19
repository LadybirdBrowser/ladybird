/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#dom-positionoptions
struct PositionOptions {
    bool enable_high_accuracy { false };
    WebIDL::UnsignedLong timeout { 0xFFFFFFFF };
    WebIDL::UnsignedLong maximum_age { 0 };
};
// https://w3c.github.io/geolocation/#geolocation_interface
class Geolocation : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Geolocation, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Geolocation);

public:
    void get_current_position(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, Optional<PositionOptions>);
    WebIDL::Long watch_position(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, Optional<PositionOptions>);
    void clear_watch(WebIDL::Long);

private:
    Geolocation(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
