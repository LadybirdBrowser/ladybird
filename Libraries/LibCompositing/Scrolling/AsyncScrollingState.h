/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibGfx/Point.h>
#include <LibWebCommon/Page/AsyncScrollNodeStableID.h>

namespace Compositing {

using AsyncScrollOperationID = u64;

struct AsyncScrollOffset {
    Web::AsyncScrollNodeStableID stable_node_id;
    Gfx::FloatPoint compositor_scroll_offset;
    Gfx::FloatPoint unadopted_scroll_delta;
    // The most recent nonzero delta of a relative scroll in each axis, such as a wheel or panning scroll. Absolute
    // scrolls, such as dragging a scrollbar thumb, leave it as it is.
    Gfx::FloatPoint last_relative_scroll_delta;
    // The scale the visual viewport was zoomed to, which only its scroll node reports.
    Optional<float> visual_viewport_scale;
    // The last of the main thread's scroll requests for the box that the compositor applied before it reported this.
    u64 applied_scroll_request_id { 0 };
    // How many times the compositor had scrolled the box for user input of its own when it reported this.
    u64 user_input_scroll_count { 0 };
    // How many times the compositor had scrolled any box of the context for user input of its own when it reported
    // this. Not every such scroll is reported, as one the next input turns back moves no box.
    u64 context_user_input_scroll_count { 0 };

    void merge_later_scroll(AsyncScrollOffset const& later)
    {
        compositor_scroll_offset = later.compositor_scroll_offset;
        applied_scroll_request_id = later.applied_scroll_request_id;
        user_input_scroll_count = later.user_input_scroll_count;
        context_user_input_scroll_count = later.context_user_input_scroll_count;
        if (later.visual_viewport_scale.has_value())
            visual_viewport_scale = later.visual_viewport_scale;
        unadopted_scroll_delta.translate_by(later.unadopted_scroll_delta);
        if (later.last_relative_scroll_delta.x() != 0)
            last_relative_scroll_delta.set_x(later.last_relative_scroll_delta.x());
        if (later.last_relative_scroll_delta.y() != 0)
            last_relative_scroll_delta.set_y(later.last_relative_scroll_delta.y());
    }
};

}
