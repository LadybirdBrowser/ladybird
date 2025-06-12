/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Window.h>

namespace Web::NavigationTiming {

class PerformanceTiming final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(PerformanceTiming, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(PerformanceTiming);

public:
    using AllowOwnPtr = TrueType;

    ~PerformanceTiming();

    u64 navigation_start()
    {
        return monotonic_timestamp_to_wall_time_milliseconds([](auto& load_info) { return load_info.navigation_start_time; });
    }
    u64 unload_event_start() { return 0; }
    u64 unload_event_end() { return 0; }
    u64 redirect_start() { return 0; }
    u64 redirect_end() { return 0; }
    u64 fetch_start() { return 0; }
    u64 domain_lookup_start() { return 0; }
    u64 domain_lookup_end() { return 0; }
    u64 connect_start() { return 0; }
    u64 connect_end() { return 0; }
    u64 secure_connection_start() { return 0; }
    u64 request_start() { return 0; }
    u64 response_start() { return 0; }
    u64 response_end() { return 0; }
    u64 dom_loading() { return 0; }
    u64 dom_interactive()
    {
        return monotonic_timestamp_to_wall_time_milliseconds([](auto& load_info) { return load_info.dom_interactive_time; });
    }
    u64 dom_content_loaded_event_start()
    {
        return monotonic_timestamp_to_wall_time_milliseconds([](auto& load_info) { return load_info.dom_content_loaded_event_start_time; });
    }
    u64 dom_content_loaded_event_end()
    {
        return monotonic_timestamp_to_wall_time_milliseconds([](auto& load_info) { return load_info.dom_content_loaded_event_end_time; });
    }
    u64 dom_complete()
    {
        return monotonic_timestamp_to_wall_time_milliseconds([](auto& load_info) { return load_info.dom_complete_time; });
    }
    u64 load_event_start()
    {
        return monotonic_timestamp_to_wall_time_milliseconds([](auto& load_info) { return load_info.load_event_start_time; });
    }
    u64 load_event_end()
    {
        return monotonic_timestamp_to_wall_time_milliseconds([](auto& load_info) { return load_info.load_event_end_time; });
    }

private:
    explicit PerformanceTiming(JS::Realm&);

    DOM::DocumentLoadTimingInfo const& document_load_timing_info(JS::Object const& global_object) const;
    u64 monotonic_timestamp_to_wall_time_milliseconds(Function<HighResolutionTime::DOMHighResTimeStamp(DOM::DocumentLoadTimingInfo const&)> selector) const;

    virtual void initialize(JS::Realm&) override;
};

}
