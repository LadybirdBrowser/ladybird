/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Point.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/ScrollNodeState.h>

namespace Web::Painting {

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

// Value store for the scroll and sticky nodes of the accumulated visual context tree: the tree
// owns structure and identity, entries here carry the offsets, sticky constraints, and the
// containing-block-derived scroll-parent references. Registration returns the entry's slot, which
// the tree walk stamps into the node's ScrollData, so resolving a node to its entry is a direct
// index in both directions. Rebuilt together with the tree; offsets are refreshed in place
// between rebuilds.
class ScrollState {
public:
    ScrollStateSlot register_scroll_node(VisualContextIndex node_index, Paintable const& paintable_box, ScrollStateSlot parent_slot)
    {
        return append_state(ScrollNodeState { node_index, paintable_box, false, parent_slot });
    }

    ScrollStateSlot register_sticky_node(VisualContextIndex node_index, Paintable const& paintable_box, ScrollStateSlot parent_slot)
    {
        return append_state(ScrollNodeState { node_index, paintable_box, true, parent_slot });
    }

    ScrollNodeState const& state_at_slot(ScrollStateSlot slot) const { return m_states_by_slot[slot.value()]; }
    ScrollNodeState& state_at_slot(ScrollStateSlot slot) { return m_states_by_slot[slot.value()]; }

    VisualContextIndex node_index_for_slot(ScrollStateSlot slot) const
    {
        if (slot == NO_SCROLL_STATE_SLOT)
            return {};
        return state_at_slot(slot).node_index();
    }

    CSSPixelPoint cumulative_offset(ScrollStateSlot slot) const
    {
        CSSPixelPoint offset;
        while (slot != NO_SCROLL_STATE_SLOT) {
            auto const& state = state_at_slot(slot);
            offset += state.own_offset();
            slot = state.parent_slot();
        }
        return offset;
    }

    CSSPixelPoint cumulative_sticky_offset(ScrollStateSlot slot) const
    {
        CSSPixelPoint offset;
        while (slot != NO_SCROLL_STATE_SLOT) {
            auto const& state = state_at_slot(slot);
            if (!state.is_sticky())
                break;
            offset += state.own_offset();
            slot = state.parent_slot();
        }
        return offset;
    }

    ScrollStateSlot nearest_scrolling_ancestor_slot(ScrollStateSlot slot) const
    {
        auto ancestor_slot = state_at_slot(slot).parent_slot();
        while (ancestor_slot != NO_SCROLL_STATE_SLOT) {
            auto const& state = state_at_slot(ancestor_slot);
            if (!state.is_sticky())
                return ancestor_slot;
            ancestor_slot = state.parent_slot();
        }
        return NO_SCROLL_STATE_SLOT;
    }

    // Iteration follows registration order, which is the tree's append order, so parent nodes are
    // always visited before their descendants.
    template<typename Callback>
    void for_each_scroll_node(Callback callback)
    {
        for (size_t slot_value = 0; slot_value < m_states_by_slot.size(); ++slot_value) {
            if (!m_states_by_slot[slot_value].is_sticky())
                callback(ScrollStateSlot { slot_value }, m_states_by_slot[slot_value]);
        }
    }

    template<typename Callback>
    void for_each_sticky_node(Callback callback)
    {
        for (size_t slot_value = 0; slot_value < m_states_by_slot.size(); ++slot_value) {
            if (m_states_by_slot[slot_value].is_sticky())
                callback(ScrollStateSlot { slot_value }, m_states_by_slot[slot_value]);
        }
    }

    void clear()
    {
        m_states_by_slot.clear_with_capacity();
    }

private:
    friend class ViewportPaintable;

    ScrollStateSlot append_state(ScrollNodeState state)
    {
        auto slot = ScrollStateSlot { m_states_by_slot.size() };
        m_states_by_slot.append(move(state));
        return slot;
    }

    ScrollStateSnapshot snapshot(double device_pixels_per_css_pixel) const;

    Vector<ScrollNodeState> m_states_by_slot;
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::ScrollStateSnapshot const&);
template<>
WEB_API ErrorOr<Web::Painting::ScrollStateSnapshot> decode(Decoder&);

}
