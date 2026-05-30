/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <Compositor/ViewportScrollbarController.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Compositor {

static Gfx::Orientation orientation_for_scrollbar(Web::Compositor::ViewportScrollbar const& scrollbar)
{
    return scrollbar.vertical ? Gfx::Orientation::Vertical : Gfx::Orientation::Horizontal;
}

struct ViewportScrollbarIdentity {
    Web::Compositor::AsyncScrollNodeID scroll_node_id;
    bool vertical { false };
};

static ViewportScrollbarIdentity viewport_scrollbar_identity(Web::Compositor::ViewportScrollbar const& scrollbar)
{
    return { scrollbar.scroll_node_id, scrollbar.vertical };
}

static Optional<ViewportScrollbarIdentity> viewport_scrollbar_identity_at(ReadonlySpan<Web::Compositor::ViewportScrollbar> scrollbars, Optional<size_t> scrollbar_index)
{
    if (!scrollbar_index.has_value())
        return {};
    return viewport_scrollbar_identity(scrollbars[*scrollbar_index]);
}

static Optional<size_t> find_viewport_scrollbar_index(ReadonlySpan<Web::Compositor::ViewportScrollbar> scrollbars, ViewportScrollbarIdentity identity)
{
    for (size_t i = 0; i < scrollbars.size(); ++i) {
        if (scrollbars[i].scroll_node_id == identity.scroll_node_id && scrollbars[i].vertical == identity.vertical)
            return i;
    }
    return {};
}

static Gfx::IntRect scrollbar_gutter_rect(Web::Compositor::ViewportScrollbar const& scrollbar, bool expanded)
{
    return expanded ? scrollbar.expanded_gutter_rect : scrollbar.gutter_rect;
}

static double scrollbar_scroll_size(Web::Compositor::ViewportScrollbar const& scrollbar, bool expanded)
{
    return expanded ? scrollbar.expanded_scroll_size : scrollbar.scroll_size;
}

static Gfx::IntRect translated_thumb_rect(Web::Compositor::ViewportScrollbar const& scrollbar, Gfx::FloatPoint scroll_offset, bool expanded)
{
    auto orientation = orientation_for_scrollbar(scrollbar);
    auto thumb_rect = expanded ? scrollbar.expanded_thumb_rect : scrollbar.thumb_rect;
    thumb_rect.translate_primary_offset_for_orientation(orientation, static_cast<int>(scroll_offset.primary_offset_for_orientation(orientation) * scrollbar_scroll_size(scrollbar, expanded)));
    return thumb_rect;
}

static Gfx::IntRect translated_thumb_rect(Web::Compositor::ViewportScrollbar const& scrollbar, Web::Painting::ScrollStateSnapshot const& scroll_state_snapshot, bool expanded)
{
    auto thumb_rect = expanded ? scrollbar.expanded_thumb_rect : scrollbar.thumb_rect;
    auto scroll_size = scrollbar_scroll_size(scrollbar, expanded);
    auto device_offset = scroll_state_snapshot.device_offset_for_index(scrollbar.scroll_frame_index);
    if (scrollbar.vertical)
        thumb_rect.translate_by(0, static_cast<int>(-device_offset.y() * scroll_size));
    else
        thumb_rect.translate_by(static_cast<int>(-device_offset.x() * scroll_size), 0);
    return thumb_rect;
}

static Gfx::IntRect scrollbar_hit_rect(Web::Compositor::ViewportScrollbar const& scrollbar, Gfx::FloatPoint scroll_offset)
{
    static constexpr int scrollbar_hit_slop = 4;

    auto rect = translated_thumb_rect(scrollbar, scroll_offset, false).united(translated_thumb_rect(scrollbar, scroll_offset, true));
    auto expanded_gutter_rect = scrollbar_gutter_rect(scrollbar, true);
    if (!expanded_gutter_rect.is_empty())
        rect.unite(expanded_gutter_rect);
    rect.inflate(scrollbar_hit_slop, scrollbar_hit_slop);
    return rect;
}

void ViewportScrollbarController::clear()
{
    m_scrollbars.clear();
    m_hovered_scrollbar_index.clear();
    m_captured_scrollbar_index.clear();
    m_thumb_grab_position = 0;
}

void ViewportScrollbarController::set_scrollbars(Vector<Web::Compositor::ViewportScrollbar> const& scrollbars)
{
    auto hovered_scrollbar_identity = viewport_scrollbar_identity_at(m_scrollbars, m_hovered_scrollbar_index);
    auto captured_scrollbar_identity = viewport_scrollbar_identity_at(m_scrollbars, m_captured_scrollbar_index);

    m_scrollbars = scrollbars;
    m_hovered_scrollbar_index = hovered_scrollbar_identity.has_value() ? find_viewport_scrollbar_index(m_scrollbars, *hovered_scrollbar_identity) : Optional<size_t> {};
    m_captured_scrollbar_index = captured_scrollbar_identity.has_value() ? find_viewport_scrollbar_index(m_scrollbars, *captured_scrollbar_identity) : Optional<size_t> {};
    if (!m_captured_scrollbar_index.has_value())
        m_thumb_grab_position = 0;
}

Optional<size_t> ViewportScrollbarController::hit_test(Web::Compositor::AsyncScrollTree const& async_scroll_tree, Web::Painting::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position) const
{
    for (size_t i = 0; i < m_scrollbars.size(); ++i) {
        auto const& scrollbar = m_scrollbars[i];
        auto scroll_offset = async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id, scroll_state_snapshot);
        if (!scroll_offset.has_value())
            continue;

        if (scrollbar_hit_rect(scrollbar, *scroll_offset).to_type<float>().contains(position))
            return i;
    }
    return {};
}

Optional<ViewportScrollbarController::Drag> ViewportScrollbarController::begin_drag(Web::Compositor::AsyncScrollTree const& async_scroll_tree, Web::Painting::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position)
{
    for (size_t i = 0; i < m_scrollbars.size(); ++i) {
        auto const& scrollbar = m_scrollbars[i];
        auto scroll_offset = async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id, scroll_state_snapshot);
        if (!scroll_offset.has_value())
            continue;

        auto expanded = is_expanded(i);
        auto orientation = orientation_for_scrollbar(scrollbar);
        auto thumb_rect = translated_thumb_rect(scrollbar, *scroll_offset, expanded);
        auto primary_position = position.primary_offset_for_orientation(orientation);
        float thumb_grab_position = 0;
        if (thumb_rect.to_type<float>().contains(position)) {
            thumb_grab_position = primary_position - static_cast<float>(thumb_rect.primary_offset_for_orientation(orientation));
        } else if (scrollbar_hit_rect(scrollbar, *scroll_offset).to_type<float>().contains(position)) {
            auto gutter_rect = scrollbar_gutter_rect(scrollbar, true);
            auto thumb_size = static_cast<float>(thumb_rect.primary_size_for_orientation(orientation));
            auto gutter_start = static_cast<float>(gutter_rect.primary_offset_for_orientation(orientation));
            auto gutter_size = static_cast<float>(gutter_rect.primary_size_for_orientation(orientation));
            auto offset_relative_to_gutter = primary_position - gutter_start;
            thumb_grab_position = max(min(offset_relative_to_gutter, thumb_size / 2), offset_relative_to_gutter - gutter_size + thumb_size);
        } else {
            continue;
        }

        m_captured_scrollbar_index = i;
        m_hovered_scrollbar_index = i;
        m_thumb_grab_position = thumb_grab_position;
        return Drag { i, primary_position, thumb_grab_position };
    }

    return {};
}

Optional<ViewportScrollbarController::Drag> ViewportScrollbarController::captured_drag(Gfx::FloatPoint position)
{
    if (!m_captured_scrollbar_index.has_value())
        return {};
    auto scrollbar_index = *m_captured_scrollbar_index;
    auto const& scrollbar = m_scrollbars[scrollbar_index];
    auto primary_position = position.primary_offset_for_orientation(orientation_for_scrollbar(scrollbar));
    return Drag { scrollbar_index, primary_position, m_thumb_grab_position };
}

Optional<ViewportScrollbarController::Drag> ViewportScrollbarController::release_captured_drag(Gfx::FloatPoint position)
{
    if (!m_captured_scrollbar_index.has_value())
        return {};
    auto scrollbar_index = *m_captured_scrollbar_index;
    auto thumb_grab_position = m_thumb_grab_position;
    auto const& scrollbar = m_scrollbars[scrollbar_index];
    auto primary_position = position.primary_offset_for_orientation(orientation_for_scrollbar(scrollbar));
    m_captured_scrollbar_index.clear();
    m_thumb_grab_position = 0;
    return Drag { scrollbar_index, primary_position, thumb_grab_position };
}

bool ViewportScrollbarController::set_hovered_scrollbar(Optional<size_t> scrollbar_index)
{
    if (m_hovered_scrollbar_index == scrollbar_index)
        return false;

    m_hovered_scrollbar_index = scrollbar_index;
    return true;
}

Optional<ViewportScrollbarController::ScrollDelta> ViewportScrollbarController::scroll_delta_for_drag(Web::Compositor::AsyncScrollTree const& async_scroll_tree, Web::Painting::ScrollStateSnapshot const& scroll_state_snapshot, Drag const& drag) const
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
    auto min_thumb_position = static_cast<float>(thumb_rect.primary_offset_for_orientation(orientation));
    auto max_thumb_position = min_thumb_position + scrollbar.max_scroll_offset * static_cast<float>(scroll_size);
    auto target_thumb_position = AK::clamp(drag.primary_position - drag.thumb_grab_position, min_thumb_position, max_thumb_position);
    auto target_scroll_offset = (target_thumb_position - min_thumb_position) / static_cast<float>(scroll_size);

    Gfx::FloatPoint delta;
    delta.set_primary_offset_for_orientation(orientation, target_scroll_offset - current_scroll_offset->primary_offset_for_orientation(orientation));
    if (delta.x() == 0 && delta.y() == 0)
        return {};

    return ScrollDelta { scrollbar.scroll_node_id, delta };
}

bool ViewportScrollbarController::paint(Gfx::PaintingSurface& surface, Web::Painting::DisplayListPlayerSkia& display_list_player, Web::Painting::ScrollStateSnapshot const& scroll_state_snapshot) const
{
    if (m_scrollbars.is_empty())
        return false;

    for (size_t i = 0; i < m_scrollbars.size(); ++i) {
        auto const& scrollbar = m_scrollbars[i];
        auto expanded = is_expanded(i);
        Web::Painting::PaintScrollBar paint_scrollbar {
            .scroll_frame_index = scrollbar.scroll_frame_index,
            .gutter_rect = scrollbar_gutter_rect(scrollbar, expanded),
            .thumb_rect = translated_thumb_rect(scrollbar, scroll_state_snapshot, expanded),
            .scroll_size = scrollbar_scroll_size(scrollbar, expanded),
            .thumb_color = scrollbar.thumb_color,
            .track_color = scrollbar.track_color,
            .vertical = scrollbar.vertical,
        };
        display_list_player.paint_scrollbar(surface, paint_scrollbar);
    }
    return true;
}

bool ViewportScrollbarController::is_expanded(size_t scrollbar_index) const
{
    return m_hovered_scrollbar_index == scrollbar_index || m_captured_scrollbar_index == scrollbar_index;
}

}
