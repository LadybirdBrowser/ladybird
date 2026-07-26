/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/NumericLimits.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/ContextRef.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

// Device-pixel offsets keyed by SpatialNodeIndex: the scroll containers' offsets as produced by
// the document, plus the sticky nodes' offsets that resolve_sticky_offsets() derives from them
// and the tree. Stored dense in process so display list replay and hit testing index it directly;
// indices that are not scroll-like nodes read as zero offsets. The IPC representation is sparse
// (index, offset) pairs.
//
// The dense store must never grow past the spatial node count. A compromised WebContent renderer controls the
// indices that reach it — both as decoded wire pairs and as scroll-node indices replayed from the display list. So,
// set_device_offset_for_index() drops any index at or past m_node_count. The compositor supplies that bound from the
// AccumulatedVisualContextTree that owns these nodes. Until it does, the bound is unlimited — which is what the
// trusted in-process snapshot built from live scroll nodes needs.
class ScrollStateSnapshot {
public:
    ReadonlySpan<Gfx::FloatPoint> device_offsets() const { return m_device_offsets; }

    Gfx::FloatPoint device_offset_for_index(SpatialNodeIndex index) const
    {
        if (index.value() >= m_device_offsets.size())
            return {};
        return m_device_offsets[index.value()];
    }

    void set_device_offset_for_index(SpatialNodeIndex index, Gfx::FloatPoint offset)
    {
        if (index.value() >= m_node_count)
            return;
        if (index.value() >= m_device_offsets.size())
            m_device_offsets.resize(index.value() + 1);
        m_device_offsets[index.value()] = offset;
    }

    // Bind the snapshot to the authoritative node count from the AccumulatedVisualContextTree that owns these nodes.
    // Wire-decoded offsets are staged rather than densified. So, this is where a decoded index is first validated:
    // staged pairs are applied through the bounded setter above, dropping any whose index is at or past the node count.
    // And any offset already stored past the count is discarded. Every later mutation stays bounded by the count.
    void set_node_count(size_t node_count)
    {
        m_node_count = node_count;
        if (m_device_offsets.size() > node_count)
            m_device_offsets.resize(node_count);
        auto staged_offsets = move(m_staged_offsets);
        for (auto const& staged : staged_offsets)
            set_device_offset_for_index(SpatialNodeIndex { staged.index }, staged.offset);
    }

private:
    template<typename T>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, T const&);
    template<typename T>
    friend ErrorOr<T> IPC::decode(IPC::Decoder&);

    // A sparse wire pair captured by the decoder before the node count is known. Held apart from the dense store — so
    // that a single decoded pair cannot grow the store to an arbitrary size.
    struct StagedOffset {
        u32 index { 0 };
        Gfx::FloatPoint offset;
    };

    Vector<Gfx::FloatPoint> m_device_offsets;
    Vector<StagedOffset> m_staged_offsets;
    size_t m_node_count { NumericLimits<size_t>::max() };
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::ScrollStateSnapshot const&);
template<>
WEB_API ErrorOr<Web::Painting::ScrollStateSnapshot> decode(Decoder&);

}
