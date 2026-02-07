/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HighResolutionTime {

DOMHighResTimeStamp estimated_monotonic_time_of_the_unix_epoch();
DOMHighResTimeStamp get_time_origin_timestamp(JS::Object const&);
DOMHighResTimeStamp coarsen_time(DOMHighResTimeStamp timestamp, HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability = HTML::CanUseCrossOriginIsolatedAPIs::No);
DOMHighResTimeStamp current_high_resolution_time(JS::Object const&);
DOMHighResTimeStamp relative_high_resolution_time(DOMHighResTimeStamp, JS::Object const&);
DOMHighResTimeStamp relative_high_resolution_coarsen_time(DOMHighResTimeStamp, JS::Object const&);
WEB_API DOMHighResTimeStamp coarsened_shared_current_time(HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability = HTML::CanUseCrossOriginIsolatedAPIs::No);
DOMHighResTimeStamp wall_clock_unsafe_current_time();
WEB_API DOMHighResTimeStamp unsafe_shared_current_time();

}
