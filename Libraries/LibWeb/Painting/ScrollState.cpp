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
    snapshot.own_offsets.ensure_capacity(scroll_frames.size());
    for (auto const& scroll_frame : scroll_frames)
        snapshot.own_offsets.append({ scroll_frame->m_own_offset });
    return snapshot;
}

}
