/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibWeb/Bindings/Geolocation.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Geolocation/GeolocationPositionError.h>
#include <LibWeb/HighResolutionTime/EpochTimeStamp.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class LocalTraversableNavigable;

}

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#dfn-emulated-position-data
using EmulatedPositionData = Variant<Empty, GC::Ref<GeolocationCoordinates>, GeolocationPositionError::ErrorCode>;

// https://w3c.github.io/geolocation/#geolocation_interface
class Geolocation : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Geolocation, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Geolocation);

public:
    void get_current_position(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, Bindings::PositionOptions const&);
    WebIDL::Long watch_position(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, Bindings::PositionOptions const&);
    void clear_watch(WebIDL::Long);

private:
    struct WatchPositionData {
        GC::Ref<WebIDL::CallbackType> success_callback;
        GC::Ptr<WebIDL::CallbackType> error_callback;
        Bindings::PositionOptions options;
        Optional<u64> emulated_position_data_observer_id {};
    };

    Geolocation(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    void acquire_a_position(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, Bindings::PositionOptions const&, Optional<WebIDL::UnsignedLong>);
    void acquire_position_from_page(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, Bindings::PositionOptions const&, Optional<WebIDL::UnsignedLong>, HighResolutionTime::EpochTimeStamp);
    void call_back_with_error(GC::Ptr<WebIDL::CallbackType>, GeolocationPositionError::ErrorCode) const;
    EmulatedPositionData get_emulated_position_data() const;
    void remove_watch_id(WebIDL::UnsignedLong);
    GC::Ptr<HTML::LocalTraversableNavigable> top_level_traversable() const;
    void request_a_position(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, Bindings::PositionOptions const&, Optional<WebIDL::UnsignedLong> = {});
    void run_in_parallel_when_document_is_visible(DOM::Document&, GC::Ref<GC::Function<void()>>);
    void unregister_watch_position_observer(WebIDL::UnsignedLong);

    // https://w3c.github.io/geolocation/#dfn-watchids
    HashTable<WebIDL::UnsignedLong> m_watch_ids;
    HashMap<WebIDL::UnsignedLong, WatchPositionData> m_watch_position_data;
    HashMap<WebIDL::UnsignedLong, u64> m_pending_watch_position_request_ids;

    // https://w3c.github.io/geolocation/#dfn-cachedposition
    GC::Ptr<GeolocationPosition> m_cached_position;

    Vector<GC::Ref<Platform::Timer>> m_timeout_timers;
};

}
