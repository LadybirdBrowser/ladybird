/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

ScrollStateSnapshot ScrollStateSnapshot::create(Vector<NonnullRefPtr<ScrollFrame>> const& scroll_frames)
{
    ScrollStateSnapshot snapshot;
    snapshot.entries.ensure_capacity(scroll_frames.size());
    for (auto const& scroll_frame : scroll_frames)
        snapshot.entries.append({ scroll_frame->cumulative_offset(), scroll_frame->own_offset() });
    return snapshot;
}

}
