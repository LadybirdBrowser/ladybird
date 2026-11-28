/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Time.h>
#include <LibWeb/HighResolutionTime/Clocks.h>

namespace Web::HighResolutionTime {

// https://w3c.github.io/hr-time/#dfn-coarsen-time
DOMHighResTimeStamp coarsen_time(DOMHighResTimeStamp timestamp, HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability)
{
    // 1. Let time resolution be 100 microseconds, or a higher implementation-defined value.
    i64 coarsening_factor = 10;

    // 2. If crossOriginIsolatedCapability is true, set time resolution to be 5 microseconds, or a higher implementation-defined value.
    if (cross_origin_isolated_capability == HTML::CanUseCrossOriginIsolatedAPIs::Yes)
        coarsening_factor = 200;

    // 3. In an implementation-defined manner, coarsen and potentially jitter timestamp such that its resolution will not exceed time resolution
    auto bucket = static_cast<i64>(floor(timestamp * static_cast<double>(coarsening_factor)));
    timestamp = static_cast<double>(bucket) / static_cast<double>(coarsening_factor);
    // NB: Division can represent the exact bucket boundary slightly below its intended value. Move such a result up
    //     by one representable value so that coarsening it again does not select the previous bucket.
    if (floor(timestamp * static_cast<double>(coarsening_factor)) < static_cast<double>(bucket))
        timestamp = nextafter(timestamp, AK::Infinity<double>);

    // FIXME: Applying jitter to the coarsened timestamp here may decrease our susceptibility to timing attacks.

    // 4. Return timestamp as a moment
    return timestamp;
}

// https://w3c.github.io/hr-time/#dfn-coarsened-shared-current-time
DOMHighResTimeStamp coarsened_shared_current_time(HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability)
{
    // The coarsened shared current time given an optional boolean crossOriginIsolatedCapability (default false), must return the result of calling coarsen time with the unsafe shared current time and crossOriginIsolatedCapability.
    return coarsen_time(unsafe_shared_current_time(), cross_origin_isolated_capability);
}

// https://w3c.github.io/hr-time/#wall-clock-unsafe-current-time
DOMHighResTimeStamp wall_clock_unsafe_current_time()
{
    return UnixDateTime::now().to_seconds_f64() * 1000.0;
}

// https://w3c.github.io/hr-time/#dfn-unsafe-shared-current-time
DOMHighResTimeStamp unsafe_shared_current_time()
{
    // The unsafe shared current time must return the current value of the shared monotonic clock.
    // Note: This is in milliseconds (stored as a double).
    return MonotonicTime::now().to_seconds_f64() * 1000.0;
}

}
