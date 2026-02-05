/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Time.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HighResolutionTime {

// https://w3c.github.io/hr-time/#dfn-estimated-monotonic-time-of-the-unix-epoch
DOMHighResTimeStamp estimated_monotonic_time_of_the_unix_epoch()
{
    // Each group of environment settings objects that could possibly communicate in any way
    // has an estimated monotonic time of the Unix epoch, a moment on the monotonic clock,
    // whose value is initialized by the following steps:

    // 1. Let wall time be the wall clock's unsafe current time.
    auto wall_time = wall_clock_unsafe_current_time();

    // 2. Let monotonic time be the monotonic clock's unsafe current time.
    auto monotonic_time = unsafe_shared_current_time();

    // 3. Let epoch time be monotonic time - (wall time - Unix epoch)
    auto epoch_time = monotonic_time - (wall_time - 0);

    // 4. Initialize the estimated monotonic time of the Unix epoch to the result of calling coarsen time with epoch time
    auto estimated_monotonic_time = coarsen_time(epoch_time);
    return estimated_monotonic_time;
}

// https://w3c.github.io/hr-time/#dfn-get-time-origin-timestamp
DOMHighResTimeStamp get_time_origin_timestamp(JS::Object const& global)
{
    // To get time origin timestamp, given a global object global, run the following steps, which return a duration:
    // 1. Let timeOrigin be global's relevant settings object's time origin.
    auto time_origin = HTML::relevant_principal_settings_object(global).time_origin();

    // 2. Return the duration from the estimated monotonic time of the Unix epoch to timeOrigin.
    return time_origin - estimated_monotonic_time_of_the_unix_epoch();
}

// https://w3c.github.io/hr-time/#dfn-coarsen-time
DOMHighResTimeStamp coarsen_time(DOMHighResTimeStamp timestamp, bool cross_origin_isolated_capability)
{
    // 1. Let time resolution be 100 microseconds, or a higher implementation-defined value.
    auto time_resolution_milliseconds = 0.1;

    // 2. If crossOriginIsolatedCapability is true, set time resolution to be 5 microseconds, or a higher implementation-defined value.
    if (cross_origin_isolated_capability)
        time_resolution_milliseconds = 0.005;

    // 3. In an implementation-defined manner, coarsen and potentially jitter timestamp such that its resolution will not exceed time resolution
    timestamp = floor(timestamp / time_resolution_milliseconds) * time_resolution_milliseconds;

    // FIXME: Applying jitter to the coarsened timestamp here may decrease our susceptibility to timing attacks.

    // 4. Return timestamp as a moment
    return timestamp;
}

// https://w3c.github.io/hr-time/#dfn-current-high-resolution-time
DOMHighResTimeStamp current_high_resolution_time(JS::Object const& global)
{
    // The current high resolution time given a global object current global must return the result
    // of relative high resolution time given unsafe shared current time and current global.
    return HighResolutionTime::relative_high_resolution_time(HighResolutionTime::unsafe_shared_current_time(), global);
}

// https://w3c.github.io/hr-time/#dfn-relative-high-resolution-time
// https://pr-preview.s3.amazonaws.com/w3c/hr-time/pull/168.html#dfn-relative-high-resolution-time
DOMHighResTimeStamp relative_high_resolution_time(DOMHighResTimeStamp time, JS::Object const& global)
{
    // 1. Let settings be the global's relevant principal settings object.
    auto& settings = HTML::relevant_principal_settings_object(global);

    // 2. Let coarse time be the result of calling coarsen time with time and settings's cross-origin isolated capability.
    auto coarse_time = coarsen_time(time, settings.cross_origin_isolated_capability() == HTML::CanUseCrossOriginIsolatedAPIs::Yes);

    // 2. Return the relative high resolution coarse time for coarse time and global.
    return relative_high_resolution_coarsen_time(coarse_time, global);
}

// https://w3c.github.io/hr-time/#dfn-relative-high-resolution-coarse-time
DOMHighResTimeStamp relative_high_resolution_coarsen_time(DOMHighResTimeStamp coarsen_time, JS::Object const& global)
{
    // The relative high resolution coarse time given a moment from the monotonic clock coarseTime and a global object global, is the duration from global's relevant settings object's time origin to coarseTime.
    auto time_origin = HTML::relevant_principal_settings_object(global).time_origin();
    return coarsen_time - time_origin;
}

// https://w3c.github.io/hr-time/#dfn-coarsened-shared-current-time
DOMHighResTimeStamp coarsened_shared_current_time(bool cross_origin_isolated_capability)
{
    // The coarsened shared current time given an optional boolean crossOriginIsolatedCapability (default false), must return the result of calling coarsen time with the unsafe shared current time and crossOriginIsolatedCapability.
    return coarsen_time(unsafe_shared_current_time(), cross_origin_isolated_capability);
}

// https://w3c.github.io/hr-time/#wall-clock-unsafe-current-time
DOMHighResTimeStamp wall_clock_unsafe_current_time()
{
    return UnixDateTime::now().nanoseconds_since_epoch() / 1.0e6;
}

// https://w3c.github.io/hr-time/#dfn-unsafe-shared-current-time
DOMHighResTimeStamp unsafe_shared_current_time()
{
    // The unsafe shared current time must return the current value of the shared monotonic clock.
    // Note: This is in milliseconds (stored as a double).
    return MonotonicTime::now().nanoseconds() / 1.0e6;
}

}
