/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <Compositor/ScrollbarController.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayListPlayerSkia.h>
#include <LibCompositing/Scrolling/AsyncScrollTree.h>
#include <LibCompositing/Scrolling/ScrollState.h>

namespace Compositor {

static Gfx::Orientation orientation_for_scrollbar(Compositing::AsyncScrollbar const& scrollbar)
{
    return scrollbar.vertical ? Gfx::Orientation::Vertical : Gfx::Orientation::Horizontal;
}

// A scroll node index can be given to another scroller by the next display list, so a scrollbar is recognized by the
// stable id of its scroller wherever both display lists name one.
struct ScrollbarIdentity {
    Optional<Compositing::AsyncScrollNodeStableID> scroller_stable_node_id;
    Compositing::AsyncScrollNodeID scroll_node_id;
    bool vertical { false };
};

static ScrollbarIdentity scrollbar_identity(Compositing::AsyncScrollbar const& scrollbar)
{
    return { scrollbar.scroller_stable_node_id, scrollbar.scroll_node_id, scrollbar.vertical };
}

static Optional<ScrollbarIdentity> scrollbar_identity_at(ReadonlySpan<Compositing::AsyncScrollbar> scrollbars, Optional<size_t> scrollbar_index)
{
    if (!scrollbar_index.has_value())
        return {};
    return scrollbar_identity(scrollbars[*scrollbar_index]);
}

static bool scrollbar_has_identity(Compositing::AsyncScrollbar const& scrollbar, ScrollbarIdentity const& identity)
{
    if (scrollbar.vertical != identity.vertical)
        return false;
    if (scrollbar.scroller_stable_node_id.has_value() && identity.scroller_stable_node_id.has_value())
        return *scrollbar.scroller_stable_node_id == *identity.scroller_stable_node_id;
    return scrollbar.scroll_node_id == identity.scroll_node_id;
}

static Optional<size_t> find_scrollbar_index(ReadonlySpan<Compositing::AsyncScrollbar> scrollbars, ScrollbarIdentity const& identity)
{
    for (size_t i = 0; i < scrollbars.size(); ++i) {
        if (scrollbar_has_identity(scrollbars[i], identity))
            return i;
    }
    return {};
}

// The rects of a scrollbar the compositor paints are in the viewport's space already.
static Optional<Gfx::FloatPoint> position_in_space_of_scrollbar(Compositing::AsyncScrollbar const& scrollbar, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position, Compositing::AccumulatedVisualContextTree::ClipBehavior clip_behavior)
{
    if (scrollbar.is_painted_by_compositor)
        return position;
    if (!visual_context_tree.context_is_valid(scrollbar.context))
        return {};
    return visual_context_tree.transform_point_for_hit_test(scrollbar.context, position, scroll_state_snapshot, clip_behavior);
}

static Gfx::IntRect scrollbar_gutter_rect(Compositing::AsyncScrollbar const& scrollbar, bool expanded)
{
    return expanded ? scrollbar.expanded_gutter_rect : scrollbar.gutter_rect;
}

static double scrollbar_scroll_size(Compositing::AsyncScrollbar const& scrollbar, bool expanded)
{
    return expanded ? scrollbar.expanded_scroll_size : scrollbar.scroll_size;
}

static Gfx::IntRect translated_thumb_rect(Compositing::AsyncScrollbar const& scrollbar, Gfx::FloatPoint scroll_offset, bool expanded)
{
    auto orientation = orientation_for_scrollbar(scrollbar);
    auto thumb_rect = expanded ? scrollbar.expanded_thumb_rect : scrollbar.thumb_rect;
    thumb_rect.translate_primary_offset_for_orientation(orientation, static_cast<int>(scroll_offset.primary_offset_for_orientation(orientation) * scrollbar_scroll_size(scrollbar, expanded)));
    return thumb_rect;
}

static Gfx::IntRect translated_thumb_rect(Compositing::AsyncScrollbar const& scrollbar, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, bool expanded)
{
    auto thumb_rect = expanded ? scrollbar.expanded_thumb_rect : scrollbar.thumb_rect;
    auto scroll_size = scrollbar_scroll_size(scrollbar, expanded);
    auto device_offset = scroll_state_snapshot.device_offset_for_index(scrollbar.scroll_node_index);
    if (scrollbar.vertical)
        thumb_rect.translate_by(0, static_cast<int>(-device_offset.y() * scroll_size));
    else
        thumb_rect.translate_by(static_cast<int>(-device_offset.x() * scroll_size), 0);
    return thumb_rect;
}

static Gfx::IntRect scrollbar_hit_rect(Compositing::AsyncScrollbar const& scrollbar, Gfx::FloatPoint scroll_offset)
{
    static constexpr int scrollbar_hit_slop = 4;

    auto rect = translated_thumb_rect(scrollbar, scroll_offset, false).united(translated_thumb_rect(scrollbar, scroll_offset, true));
    auto expanded_gutter_rect = scrollbar_gutter_rect(scrollbar, true);
    if (!expanded_gutter_rect.is_empty())
        rect.unite(expanded_gutter_rect);
    rect.inflate(scrollbar_hit_slop, scrollbar_hit_slop);
    return rect;
}

void ScrollbarController::clear()
{
    m_scrollbars.clear();
    m_hovered_scrollbar_index.clear();
    m_captured_scrollbar_index.clear();
    m_thumb_grab_position = 0;
    m_last_primary_position_of_captured_drag = 0;
}

void ScrollbarController::set_scrollbars(Vector<Compositing::AsyncScrollbar> const& scrollbars)
{
    auto hovered_scrollbar_identity = scrollbar_identity_at(m_scrollbars, m_hovered_scrollbar_index);
    auto captured_scrollbar_identity = scrollbar_identity_at(m_scrollbars, m_captured_scrollbar_index);

    m_scrollbars = scrollbars;
    m_hovered_scrollbar_index = hovered_scrollbar_identity.has_value() ? find_scrollbar_index(m_scrollbars, *hovered_scrollbar_identity) : Optional<size_t> {};
    m_captured_scrollbar_index = captured_scrollbar_identity.has_value() ? find_scrollbar_index(m_scrollbars, *captured_scrollbar_identity) : Optional<size_t> {};
    if (!m_captured_scrollbar_index.has_value())
        m_thumb_grab_position = 0;
}

Optional<Compositing::ScrollbarDraggedByCompositor> ScrollbarController::captured_scrollbar_painted_by_display_list() const
{
    if (!m_captured_scrollbar_index.has_value())
        return {};
    auto const& scrollbar = m_scrollbars[*m_captured_scrollbar_index];
    if (scrollbar.is_painted_by_compositor || !scrollbar.scroller_stable_node_id.has_value())
        return {};
    return Compositing::ScrollbarDraggedByCompositor {
        .scroller_stable_node_id = *scrollbar.scroller_stable_node_id,
        .vertical = scrollbar.vertical,
    };
}

Optional<size_t> ScrollbarController::hit_test_scrollbar_painted_by_compositor(Compositing::AsyncScrollTree const& async_scroll_tree, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position) const
{
    for (size_t i = 0; i < m_scrollbars.size(); ++i) {
        auto const& scrollbar = m_scrollbars[i];
        if (!scrollbar.is_painted_by_compositor)
            continue;
        auto scroll_offset = async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id, scroll_state_snapshot);
        if (!scroll_offset.has_value())
            continue;

        if (scrollbar_hit_rect(scrollbar, *scroll_offset).to_type<float>().contains(position))
            return i;
    }
    return {};
}

// A scrollbar the display list paints takes a press only where the main thread would give it one: inside the rect of
// the scrollbar as it is painted now, and with nothing that takes pointer input painted over it there. Any other press
// is left to the main thread.
Optional<size_t> ScrollbarController::hit_test_scrollbar_painted_by_display_list(Compositing::AsyncScrollTree const& async_scroll_tree, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position) const
{
    for (size_t i = m_scrollbars.size(); i-- > 0;) {
        auto const& scrollbar = m_scrollbars[i];
        if (scrollbar.is_painted_by_compositor || !scrollbar.scroller_stable_node_id.has_value())
            continue;
        if (!async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id, scroll_state_snapshot).has_value())
            continue;

        auto position_in_scrollbar_space = position_in_space_of_scrollbar(scrollbar, visual_context_tree, scroll_state_snapshot, position, Compositing::AccumulatedVisualContextTree::ClipBehavior::Respect);
        if (!position_in_scrollbar_space.has_value())
            continue;
        auto const& painted_scrollbar_rect = scrollbar.display_list_paints_enlarged_scrollbar ? scrollbar.expanded_gutter_rect : scrollbar.track_rect;
        if (!painted_scrollbar_rect.to_type<float>().contains(*position_in_scrollbar_space))
            continue;

        if (async_scroll_tree.is_covered_by_hit_test_target_painted_after(scrollbar.paint_order_index, visual_context_tree, position))
            return {};
        return i;
    }
    return {};
}

Optional<ScrollbarController::Drag> ScrollbarController::begin_drag(Compositing::AsyncScrollTree const& async_scroll_tree, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position)
{
    auto scrollbar_index = hit_test_scrollbar_painted_by_compositor(async_scroll_tree, scroll_state_snapshot, position);
    if (!scrollbar_index.has_value())
        scrollbar_index = hit_test_scrollbar_painted_by_display_list(async_scroll_tree, visual_context_tree, scroll_state_snapshot, position);
    if (!scrollbar_index.has_value())
        return {};

    auto const& scrollbar = m_scrollbars[*scrollbar_index];
    auto scroll_offset = async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id, scroll_state_snapshot);
    VERIFY(scroll_offset.has_value());
    auto position_in_scrollbar_space = position_in_space_of_scrollbar(scrollbar, visual_context_tree, scroll_state_snapshot, position, Compositing::AccumulatedVisualContextTree::ClipBehavior::Respect);
    VERIFY(position_in_scrollbar_space.has_value());

    auto orientation = orientation_for_scrollbar(scrollbar);

    // A successful press captures and expands the scrollbar before its drag delta is applied.
    static constexpr auto expanded = true;
    auto thumb_rect = translated_thumb_rect(scrollbar, *scroll_offset, expanded);
    auto thumb_hit_rect = thumb_rect.to_type<float>();

    auto primary_position = position_in_scrollbar_space->primary_offset_for_orientation(orientation);
    auto position_is_along_thumb = orientation == Gfx::Orientation::Vertical
        ? thumb_hit_rect.contains_vertically(primary_position)
        : thumb_hit_rect.contains_horizontally(primary_position);

    float thumb_grab_position = 0;
    if (position_is_along_thumb) {
        thumb_grab_position = primary_position - static_cast<float>(thumb_rect.primary_offset_for_orientation(orientation));
    } else {
        auto gutter_rect = scrollbar_gutter_rect(scrollbar, true);
        auto thumb_size = static_cast<float>(thumb_rect.primary_size_for_orientation(orientation));
        auto gutter_start = static_cast<float>(gutter_rect.primary_offset_for_orientation(orientation));
        auto gutter_size = static_cast<float>(gutter_rect.primary_size_for_orientation(orientation));
        auto offset_relative_to_gutter = primary_position - gutter_start;
        thumb_grab_position = max(min(offset_relative_to_gutter, thumb_size / 2), offset_relative_to_gutter - gutter_size + thumb_size);
    }

    m_captured_scrollbar_index = *scrollbar_index;
    // The main thread expands a scrollbar the display list paints, on the press it still receives.
    if (scrollbar.is_painted_by_compositor)
        m_hovered_scrollbar_index = *scrollbar_index;
    m_thumb_grab_position = thumb_grab_position;
    m_last_primary_position_of_captured_drag = primary_position;
    return Drag { *scrollbar_index, primary_position, thumb_grab_position };
}

// A drag usually leaves the clip of its scrollbar, and the spaces above the scrollbar may move while it lasts, so the
// position is mapped anew for every event without regard to clips. A position that cannot be mapped moves nothing.
float ScrollbarController::primary_position_of_captured_drag(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position)
{
    auto const& scrollbar = m_scrollbars[*m_captured_scrollbar_index];
    auto position_in_scrollbar_space = position_in_space_of_scrollbar(scrollbar, visual_context_tree, scroll_state_snapshot, position, Compositing::AccumulatedVisualContextTree::ClipBehavior::Ignore);
    if (position_in_scrollbar_space.has_value())
        m_last_primary_position_of_captured_drag = position_in_scrollbar_space->primary_offset_for_orientation(orientation_for_scrollbar(scrollbar));
    return m_last_primary_position_of_captured_drag;
}

Optional<ScrollbarController::Drag> ScrollbarController::captured_drag(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position)
{
    if (!m_captured_scrollbar_index.has_value())
        return {};
    auto primary_position = primary_position_of_captured_drag(visual_context_tree, scroll_state_snapshot, position);
    return Drag { *m_captured_scrollbar_index, primary_position, m_thumb_grab_position };
}

Optional<ScrollbarController::Drag> ScrollbarController::release_captured_drag(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position)
{
    if (!m_captured_scrollbar_index.has_value())
        return {};
    auto scrollbar_index = *m_captured_scrollbar_index;
    auto thumb_grab_position = m_thumb_grab_position;
    auto primary_position = primary_position_of_captured_drag(visual_context_tree, scroll_state_snapshot, position);
    m_captured_scrollbar_index.clear();
    m_thumb_grab_position = 0;
    return Drag { scrollbar_index, primary_position, thumb_grab_position };
}

bool ScrollbarController::set_hovered_scrollbar(Optional<size_t> scrollbar_index)
{
    if (m_hovered_scrollbar_index == scrollbar_index)
        return false;

    m_hovered_scrollbar_index = scrollbar_index;
    return true;
}

Optional<ScrollbarController::ScrollOffset> ScrollbarController::scroll_offset_for_drag(Compositing::AsyncScrollTree const& async_scroll_tree, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Drag const& drag) const
{
    auto const& scrollbar = m_scrollbars[drag.scrollbar_index];
    auto expanded = is_expanded(drag.scrollbar_index);
    auto scroll_size = scrollbar_scroll_size(scrollbar, expanded);
    if (scroll_size == 0)
        return {};

    auto current_scroll_offset = async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id, scroll_state_snapshot);
    if (!current_scroll_offset.has_value())
        return {};

    auto orientation = orientation_for_scrollbar(scrollbar);
    auto thumb_rect = expanded ? scrollbar.expanded_thumb_rect : scrollbar.thumb_rect;
    auto zero_offset_thumb_position = static_cast<float>(thumb_rect.primary_offset_for_orientation(orientation));
    auto min_thumb_position = zero_offset_thumb_position + scrollbar.min_scroll_offset * static_cast<float>(scroll_size);
    auto max_thumb_position = zero_offset_thumb_position + scrollbar.max_scroll_offset * static_cast<float>(scroll_size);
    auto target_thumb_position = AK::clamp(drag.primary_position - drag.thumb_grab_position, min_thumb_position, max_thumb_position);
    auto target_scroll_offset = (target_thumb_position - zero_offset_thumb_position) / static_cast<float>(scroll_size);
    // A thumb at either end of its track scrolls exactly to that end, whatever rounding the division above suffers.
    if (target_thumb_position == min_thumb_position)
        target_scroll_offset = scrollbar.min_scroll_offset;
    else if (target_thumb_position == max_thumb_position)
        target_scroll_offset = scrollbar.max_scroll_offset;

    auto scroll_offset = *current_scroll_offset;
    scroll_offset.set_primary_offset_for_orientation(orientation, target_scroll_offset);
    return ScrollOffset { scrollbar.scroll_node_id, scroll_offset };
}

bool ScrollbarController::paint(Gfx::PaintingSurface& surface, Compositing::DisplayListPlayerSkia& display_list_player, Compositing::ScrollStateSnapshot const& scroll_state_snapshot) const
{
    bool painted_a_scrollbar = false;
    for (size_t i = 0; i < m_scrollbars.size(); ++i) {
        auto const& scrollbar = m_scrollbars[i];
        if (!scrollbar.is_painted_by_compositor)
            continue;
        auto expanded = is_expanded(i);
        Compositing::PaintScrollBar paint_scrollbar {
            .scroll_node_index = scrollbar.scroll_node_index,
            .gutter_rect = scrollbar_gutter_rect(scrollbar, expanded),
            .thumb_rect = translated_thumb_rect(scrollbar, scroll_state_snapshot, expanded),
            .track_rect = scrollbar_gutter_rect(scrollbar, true),
            .scroll_size = scrollbar_scroll_size(scrollbar, expanded),
            .thumb_color = scrollbar.thumb_color,
            .track_color = scrollbar.track_color,
            .vertical = scrollbar.vertical,
        };
        display_list_player.paint_scrollbar(surface, paint_scrollbar);
        painted_a_scrollbar = true;
    }
    return painted_a_scrollbar;
}

bool ScrollbarController::is_expanded(size_t scrollbar_index) const
{
    return m_hovered_scrollbar_index == scrollbar_index || m_captured_scrollbar_index == scrollbar_index;
}

}
