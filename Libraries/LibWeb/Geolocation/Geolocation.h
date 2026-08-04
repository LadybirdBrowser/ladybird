/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Geolocation.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Geolocation/GeolocationPositionError.h>
#include <LibWeb/HighResolutionTime/EpochTimeStamp.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Bindings {

struct PositionOptions;

}

namespace Web::Geolocation {

// https://w3c.github.io/geolocation/#dfn-emulated-position-data
using EmulatedPositionData = Variant<Empty, GC::Ref<GeolocationCoordinates>, GeolocationPositionError::ErrorCode>;

using PositionOptions = Bindings::PositionOptions;

struct WatchPositionData {
    GC::Ref<WebIDL::CallbackType> success_callback;
    GC::Ptr<WebIDL::CallbackType> error_callback;
    PositionOptions options;
    Optional<u64> emulated_position_data_observer_id {};
};

// https://w3c.github.io/geolocation/#geolocation_interface
class Geolocation : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Geolocation, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Geolocation);

public:
    [[nodiscard]] static GC::Ref<Geolocation> create(HTML::Window&);

    void get_current_position(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, PositionOptions const&);
    WebIDL::Long watch_position(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, PositionOptions const&);
    void clear_watch(WebIDL::Long);

private:
    explicit Geolocation(HTML::Window&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    HTML::Window& window() const { return m_window; }

    void acquire_a_position(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, PositionOptions const&, Optional<WebIDL::UnsignedLong>);
    void acquire_position_from_page(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, PositionOptions const&, Optional<WebIDL::UnsignedLong>, HighResolutionTime::EpochTimeStamp);
    void call_back_with_error(GC::Ptr<WebIDL::CallbackType>, GeolocationPositionError::ErrorCode) const;
    EmulatedPositionData get_emulated_position_data() const;
    void remove_watch_id(WebIDL::UnsignedLong);
    GC::Ptr<HTML::LocalTraversableNavigable> top_level_traversable() const;
    void request_a_position(GC::Ref<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>, PositionOptions const&, Optional<WebIDL::UnsignedLong>);
    void run_in_parallel_when_document_is_visible(DOM::Document&, GC::Ref<GC::Function<void()>>);
    void unregister_watch_position_observer(WebIDL::UnsignedLong);

    // https://w3c.github.io/geolocation/#dfn-watchids
    HashTable<WebIDL::UnsignedLong> m_watch_ids;
    HashMap<WebIDL::UnsignedLong, WatchPositionData> m_watch_position_data;
    HashMap<WebIDL::UnsignedLong, u64> m_pending_watch_position_request_ids;

    GC::Ref<HTML::Window> m_window;

    // https://w3c.github.io/geolocation/#dfn-cachedposition
    GC::Ptr<GeolocationPosition> m_cached_position;

    Vector<GC::Ref<Platform::Timer>> m_timeout_timers;
};

}
