/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/TimelineRange.h>
#include <LibWeb/ComputedValuesRustFFI.h>

namespace Web::Animations {

static double length_percentage_px(CSS::RustStyleValueHandle const& calculated, double px, double percentage, double basis)
{
    if (calculated)
        return CSS::ComputedValuesFFI::rust_length_percentage_px(calculated.data(), basis);
    return px + percentage / 100 * basis;
}

CSS::RustStyleValueHandle retain_calculated_offset(void const* value)
{
    if (!value)
        return {};
    return CSS::RustStyleValueHandle::retained(static_cast<CSS::StyleValueFFI::StyleValueData const*>(value));
}

double TimelineRangeEdge::offset_along(double range_size) const
{
    return length_percentage_px(calculated_offset, offset_px, offset_percentage, range_size);
}

double ViewTimelineInsetEdge::resolve(double scrollport_size) const
{
    VERIFY(!is_auto);
    return length_percentage_px(calculated, px, percentage, scrollport_size);
}

}
