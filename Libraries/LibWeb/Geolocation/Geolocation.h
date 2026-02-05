/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Geolocation/GeolocationPositionError.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#dom-positionoptions
struct PositionOptions {
    bool enable_high_accuracy { false };
    WebIDL::UnsignedLong timeout { 0xFFFFFFFF };
    WebIDL::UnsignedLong maximum_age { 0 };
};

// https://w3c.github.io/geolocation/#dfn-emulated-position-data
using EmulatedPositionData = Variant<Empty, GC::Ref<GeolocationCoordinates>, GeolocationPositionError::ErrorCode>;

// https://w3c.github.io/geolocation/#geolocation_interface
class Geolocation : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Geolocation, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Geolocation);

public:
    void get_current_position(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, PositionOptions);
    WebIDL::Long watch_position(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, PositionOptions);
    void clear_watch(WebIDL::Long);

private:
    Geolocation(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    void acquire_a_position(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, PositionOptions, Optional<WebIDL::UnsignedLong>);
    void call_back_with_error(GC::Ptr<WebIDL::CallbackType>, GeolocationPositionError::ErrorCode) const;
    EmulatedPositionData get_emulated_position_data() const;
    void request_a_position(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, PositionOptions, Optional<WebIDL::UnsignedLong> = {});
    void run_in_parallel_when_document_is_visible(DOM::Document&, GC::Ref<GC::Function<void()>>);

    // https://w3c.github.io/geolocation/#dfn-watchids
    HashTable<WebIDL::UnsignedLong> m_watch_ids;

    // https://w3c.github.io/geolocation/#dfn-cachedposition
    GC::Ptr<GeolocationPosition> m_cached_position;

    Vector<GC::Ref<Platform::Timer>> m_timeout_timers;
};

}
