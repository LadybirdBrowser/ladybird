/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWebCommon/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HighResolutionTime {

WEB_API DOMHighResTimeStamp coarsen_time(DOMHighResTimeStamp timestamp, HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability = HTML::CanUseCrossOriginIsolatedAPIs::No);
WEB_API DOMHighResTimeStamp coarsened_shared_current_time(HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability = HTML::CanUseCrossOriginIsolatedAPIs::No);
DOMHighResTimeStamp wall_clock_unsafe_current_time();
WEB_API DOMHighResTimeStamp unsafe_shared_current_time();

}
