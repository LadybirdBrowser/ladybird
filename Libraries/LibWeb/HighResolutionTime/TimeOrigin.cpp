/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HighResolutionTime {

extern "C" {
double web_high_resolution_time_current_high_resolution_time(JSObject* global_object);
double web_high_resolution_time_get_time_origin_timestamp(JSObject* global_object);
double web_high_resolution_time_estimated_monotonic_time_of_the_unix_epoch();
double web_high_resolution_time_coarsen_time(double timestamp);
}

// https://w3c.github.io/hr-time/#dfn-estimated-monotonic-time-of-the-unix-epoch
DOMHighResTimeStamp estimated_monotonic_time_of_the_unix_epoch()
{
    // Each group of environment settings objects that could possibly communicate in any way
    // has an estimated monotonic time of the Unix epoch, a moment on the monotonic clock,
    // whose value is initialized by the following steps:
    // AD-HOC: The estimate is initialized once per process, since nothing shares it between processes, though
    //         environment settings objects in different processes can communicate.
    static DOMHighResTimeStamp const estimated_monotonic_time = [] {
        // 1. Let wall time be the wall clock's unsafe current time.
        auto wall_time = wall_clock_unsafe_current_time();

        // 2. Let monotonic time be the monotonic clock's unsafe current time.
        auto monotonic_time = unsafe_shared_current_time();

        // 3. Let epoch time be monotonic time - (wall time - Unix epoch)
        auto epoch_time = monotonic_time - (wall_time - 0);

        // 4. Initialize the estimated monotonic time of the Unix epoch to the result of calling coarsen time with
        //    epoch time
        return coarsen_time(epoch_time);
    }();
    return estimated_monotonic_time;
}

// https://w3c.github.io/hr-time/#dfn-get-time-origin-timestamp
DOMHighResTimeStamp get_time_origin_timestamp(JS::Object const& global)
{
    // To get time origin timestamp, given a global object global, run the following steps, which return a duration:
    // 1. Let timeOrigin be global's relevant settings object's time origin.
    auto time_origin = HTML::relevant_settings_object(global).time_origin();

    // 2. Return the duration from the estimated monotonic time of the Unix epoch to timeOrigin.
    return time_origin - estimated_monotonic_time_of_the_unix_epoch();
}

// https://w3c.github.io/hr-time/#dfn-current-high-resolution-time
DOMHighResTimeStamp current_high_resolution_time(JS::Object const& global)
{
    // The current high resolution time given a global object current global must return the result
    // of relative high resolution time given unsafe shared current time and current global.
    return HighResolutionTime::relative_high_resolution_time(HighResolutionTime::unsafe_shared_current_time(), global);
}

// https://w3c.github.io/hr-time/#dfn-relative-high-resolution-time
DOMHighResTimeStamp relative_high_resolution_time(DOMHighResTimeStamp time, JS::Object const& global)
{
    // 1. Let coarse time be the result of calling coarsen time with time and global’s relevant settings object’s cross-origin isolated capability.
    auto coarse_time = coarsen_time(time, HTML::relevant_settings_object(global).cross_origin_isolated_capability());

    // 2. Return the relative high resolution coarse time for coarse time and global.
    return relative_high_resolution_coarsen_time(coarse_time, global);
}

// https://w3c.github.io/hr-time/#dfn-relative-high-resolution-coarse-time
DOMHighResTimeStamp relative_high_resolution_coarsen_time(DOMHighResTimeStamp coarsen_time, JS::Object const& global)
{
    // The relative high resolution coarse time given a moment from the monotonic clock coarseTime and a global object global, is the duration from global's relevant settings object's time origin to coarseTime.
    auto time_origin = HTML::relevant_settings_object(global).time_origin();
    return coarsen_time - time_origin;
}

extern "C" {

double web_high_resolution_time_current_high_resolution_time(JSObject* global_object)
{
    return current_high_resolution_time(*JS::HostABI::object_from_abi(global_object));
}

double web_high_resolution_time_get_time_origin_timestamp(JSObject* global_object)
{
    return get_time_origin_timestamp(*JS::HostABI::object_from_abi(global_object));
}

double web_high_resolution_time_estimated_monotonic_time_of_the_unix_epoch()
{
    return estimated_monotonic_time_of_the_unix_epoch();
}

double web_high_resolution_time_coarsen_time(double timestamp)
{
    return coarsen_time(timestamp);
}
}

}
