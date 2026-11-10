/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibCompositing/Types.h>

namespace Compositing {

COMPOSITING_API String async_scrolling_state_as_json(DisplayList const*);
COMPOSITING_API FirstWheelStepRouteForTesting route_first_wheel_step_through_recorded_display_list(DisplayList const&, AccumulatedVisualContextTree const&, ScrollStateSnapshot const&, u64 newest_wheel_event_listener_state_generation, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels);
COMPOSITING_API bool recorded_display_list_blocks_wheel_event_at(DisplayList const&, AccumulatedVisualContextTree const&, ScrollStateSnapshot const&, Gfx::FloatPoint position);

}
