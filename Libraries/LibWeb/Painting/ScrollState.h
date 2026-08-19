/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/NumericLimits.h>
#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/VisualContextIndex.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, ScrollStateSlot);
static constexpr ScrollStateSlot NO_SCROLL_STATE_SLOT { NumericLimits<size_t>::max() };

struct StickyInsets {
    Optional<CSSPixels> top;
    Optional<CSSPixels> right;
    Optional<CSSPixels> bottom;
    Optional<CSSPixels> left;
};

// Device-pixel scroll offsets keyed by the scroll node's VisualContextIndex. Stored dense in
// process so display list replay and hit testing index it directly; indices that are not scroll
// nodes read as zero offsets. The IPC representation is sparse (index, offset) pairs.
class ScrollStateSnapshot {
public:
    ReadonlySpan<Gfx::FloatPoint> device_offsets() const { return m_device_offsets; }

    Gfx::FloatPoint device_offset_for_index(VisualContextIndex index) const
    {
        if (index.value() >= m_device_offsets.size())
            return {};
        return m_device_offsets[index.value()];
    }

    void set_device_offset_for_index(VisualContextIndex index, Gfx::FloatPoint offset)
    {
        if (index.value() >= m_device_offsets.size())
            m_device_offsets.resize(index.value() + 1);
        m_device_offsets[index.value()] = offset;
    }

private:
    Vector<Gfx::FloatPoint> m_device_offsets;
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::ScrollStateSnapshot const&);
template<>
WEB_API ErrorOr<Web::Painting::ScrollStateSnapshot> decode(Decoder&);

}
