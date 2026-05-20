/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Font.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaUtils.h>
#include <LibWeb/Compositor/CompositorContextState.h>
#include <LibWeb/Painting/DisplayList.h>

#include <AK/Assertions.h>
#include <AK/StdLibExtras.h>

#include <core/SkCanvas.h>
#include <core/SkPaint.h>
#include <core/SkRRect.h>

namespace Web::Compositor {

static SkRect to_skia_rect(Gfx::IntRect const& rect)
{
    return SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height());
}

static Gfx::Orientation orientation_for_scrollbar(ViewportScrollbar const& scrollbar)
{
    return scrollbar.vertical ? Gfx::Orientation::Vertical : Gfx::Orientation::Horizontal;
}

struct ViewportScrollbarIdentity {
    AsyncScrollNodeID scroll_node_id;
    bool vertical { false };
};

static ViewportScrollbarIdentity viewport_scrollbar_identity(ViewportScrollbar const& scrollbar)
{
    return { scrollbar.scroll_node_id, scrollbar.vertical };
}

static Optional<ViewportScrollbarIdentity> viewport_scrollbar_identity_at(ReadonlySpan<ViewportScrollbar> scrollbars, Optional<size_t> scrollbar_index)
{
    if (!scrollbar_index.has_value() || *scrollbar_index >= scrollbars.size())
        return {};
    return viewport_scrollbar_identity(scrollbars[*scrollbar_index]);
}

static Optional<size_t> find_viewport_scrollbar_index(ReadonlySpan<ViewportScrollbar> scrollbars, ViewportScrollbarIdentity identity)
{
    for (size_t i = 0; i < scrollbars.size(); ++i) {
        if (scrollbars[i].scroll_node_id == identity.scroll_node_id && scrollbars[i].vertical == identity.vertical)
            return i;
    }
    return {};
}

static void set_or_append_pending_scroll_offset(Vector<AsyncScrollOffset>& pending_scroll_offsets, AsyncScrollOffset const& scroll_offset)
{
    for (auto& existing : pending_scroll_offsets) {
        if (existing.stable_node_id == scroll_offset.stable_node_id) {
            existing.compositor_scroll_offset = scroll_offset.compositor_scroll_offset;
            existing.unadopted_scroll_delta.translate_by(scroll_offset.unadopted_scroll_delta);
            return;
        }
    }
    pending_scroll_offsets.append(scroll_offset);
}

static Gfx::IntRect scrollbar_gutter_rect(ViewportScrollbar const& scrollbar, bool expanded)
{
    return expanded ? scrollbar.expanded_gutter_rect : scrollbar.gutter_rect;
}

static double scrollbar_scroll_size(ViewportScrollbar const& scrollbar, bool expanded)
{
    return expanded ? scrollbar.expanded_scroll_size : scrollbar.scroll_size;
}

static Gfx::IntRect translated_thumb_rect(ViewportScrollbar const& scrollbar, Painting::ScrollStateSnapshot const& scroll_state_snapshot, bool expanded)
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

static Gfx::IntRect translated_thumb_rect(ViewportScrollbar const& scrollbar, Gfx::FloatPoint scroll_offset, bool expanded)
{
    auto orientation = orientation_for_scrollbar(scrollbar);
    auto thumb_rect = expanded ? scrollbar.expanded_thumb_rect : scrollbar.thumb_rect;
    thumb_rect.translate_primary_offset_for_orientation(orientation, static_cast<int>(scroll_offset.primary_offset_for_orientation(orientation) * scrollbar_scroll_size(scrollbar, expanded)));
    return thumb_rect;
}

static Gfx::IntRect scrollbar_hit_rect(ViewportScrollbar const& scrollbar, Gfx::FloatPoint scroll_offset)
{
    static constexpr int scrollbar_hit_slop = 4;

    auto rect = translated_thumb_rect(scrollbar, scroll_offset, false).united(translated_thumb_rect(scrollbar, scroll_offset, true));
    auto expanded_gutter_rect = scrollbar_gutter_rect(scrollbar, true);
    if (!expanded_gutter_rect.is_empty())
        rect.unite(expanded_gutter_rect);
    rect.inflate(scrollbar_hit_slop, scrollbar_hit_slop);
    return rect;
}

static void paint_viewport_scrollbar(Gfx::PaintingSurface& surface, ViewportScrollbar const& scrollbar, Painting::ScrollStateSnapshot const& scroll_state_snapshot, bool expanded)
{
    auto thumb_rect = translated_thumb_rect(scrollbar, scroll_state_snapshot, expanded);
    auto& canvas = surface.canvas();

    SkPaint gutter_fill_paint;
    gutter_fill_paint.setColor(to_skia_color(scrollbar.track_color));
    canvas.drawRect(to_skia_rect(scrollbar_gutter_rect(scrollbar, expanded)), gutter_fill_paint);

    auto skia_thumb_rect = to_skia_rect(thumb_rect);
    auto radius = skia_thumb_rect.width() / 2;
    auto thumb_rrect = SkRRect::MakeRectXY(skia_thumb_rect, radius, radius);

    SkPaint thumb_fill_paint;
    thumb_fill_paint.setColor(to_skia_color(scrollbar.thumb_color));
    canvas.drawRRect(thumb_rrect, thumb_fill_paint);

    SkPaint stroke_paint;
    stroke_paint.setStroke(true);
    stroke_paint.setStrokeWidth(1);
    stroke_paint.setAntiAlias(true);
    stroke_paint.setColor(to_skia_color(scrollbar.thumb_color.lightened()));
    canvas.drawRRect(thumb_rrect, stroke_paint);
}

static void paint_viewport_scrollbars(Gfx::PaintingSurface& surface, ReadonlySpan<ViewportScrollbar> scrollbars, Painting::ScrollStateSnapshot const& scroll_state_snapshot, Optional<size_t> hovered_scrollbar_index, Optional<size_t> captured_scrollbar_index)
{
    for (size_t i = 0; i < scrollbars.size(); ++i)
        paint_viewport_scrollbar(surface, scrollbars[i], scroll_state_snapshot, hovered_scrollbar_index == i || captured_scrollbar_index == i);
}

CompositorContextState::CompositorContextState(Optional<u64> page_id, PagePresentationRegistration page_presentation_registration)
    : presents_to_client(page_presentation_registration == PagePresentationRegistration::Yes)
    , page_id(page_id)
{
    VERIFY(!presents_to_client || page_id.has_value());
}

CompositorContextState::~CompositorContextState() = default;

AsyncScrollOperationID CompositorContextState::next_async_scroll_operation_id()
{
    return ++next_async_scroll_operation_id_value;
}

void CompositorContextState::record_completed_async_scroll_operation(Optional<AsyncScrollOperationID> operation_id)
{
    if (operation_id.has_value())
        completed_async_scroll_operation_ids.append(*operation_id);
}

bool CompositorContextState::has_pending_async_scroll_updates() const
{
    return !pending_async_scroll_offsets.is_empty()
        || !completed_async_scroll_operation_ids.is_empty();
}

PendingAsyncScrollUpdates CompositorContextState::take_pending_async_scroll_updates()
{
    PendingAsyncScrollUpdates updates;
    AK::swap(updates.scroll_offsets, pending_async_scroll_offsets);
    AK::swap(updates.completed_operation_ids, completed_async_scroll_operation_ids);
    return updates;
}

void CompositorContextState::store_pending_async_scroll_offsets(Vector<AsyncScrollOffset> const& scroll_offsets, Optional<AsyncScrollOperationID> operation_id)
{
    for (auto const& scroll_offset : scroll_offsets)
        set_or_append_pending_scroll_offset(pending_async_scroll_offsets, scroll_offset);
    record_completed_async_scroll_operation(operation_id);
}

Optional<Gfx::FloatPoint> CompositorContextState::reapply_pending_async_scroll_offsets(Vector<AsyncScrollOffset> const& pending_scroll_offsets)
{
    Optional<Gfx::FloatPoint> viewport_scroll_offset;
    for (auto const& pending_scroll_offset : pending_scroll_offsets) {
        auto node_id = async_scroll_tree.scroll_node_id_for_stable_id(pending_scroll_offset.stable_node_id);
        if (!node_id.has_value())
            continue;
        auto current_scroll_offset = async_scroll_tree.scroll_offset_for_node(*node_id, cached_scroll_state_snapshot);
        if (!current_scroll_offset.has_value())
            continue;
        // Reapplying pending async offsets is a restoration step for a freshly received
        // main-thread snapshot. The pending offset is already the compositor-visible
        // position, so applying the unadopted delta here would compound it.
        auto reconciled_scroll_offset = async_scroll_tree.set_scroll_offset(*node_id, pending_scroll_offset.compositor_scroll_offset, cached_scroll_state_snapshot);
        if (reconciled_scroll_offset.has_value() && async_scroll_tree.scroll_node_is_viewport(*node_id))
            viewport_scroll_offset = *reconciled_scroll_offset;
    }
    return viewport_scroll_offset;
}

void CompositorContextState::set_async_scrolling_state(AsyncScrollingState&& async_scrolling_state)
{
    auto hovered_scrollbar_identity = viewport_scrollbar_identity_at(viewport_scrollbars, hovered_viewport_scrollbar_index);
    auto captured_scrollbar_identity = viewport_scrollbar_identity_at(viewport_scrollbars, captured_viewport_scrollbar_index);
    viewport_scrollbars = move(async_scrolling_state.viewport_scrollbars);
    hovered_viewport_scrollbar_index = hovered_scrollbar_identity.has_value() ? find_viewport_scrollbar_index(viewport_scrollbars, *hovered_scrollbar_identity) : Optional<size_t> {};
    captured_viewport_scrollbar_index = captured_scrollbar_identity.has_value() ? find_viewport_scrollbar_index(viewport_scrollbars, *captured_scrollbar_identity) : Optional<size_t> {};
    async_scroll_tree.set_state(move(async_scrolling_state));
}

WheelHitTestResult CompositorContextState::hit_test_scroll_node_for_wheel(Gfx::FloatPoint position, Gfx::FloatPoint delta) const
{
    return async_scroll_tree.hit_test_scroll_node_for_wheel(position, delta);
}

Optional<size_t> CompositorContextState::hit_test_viewport_scrollbar(Gfx::FloatPoint position) const
{
    for (size_t i = 0; i < viewport_scrollbars.size(); ++i) {
        auto const& scrollbar = viewport_scrollbars[i];
        auto scroll_offset = async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id, cached_scroll_state_snapshot);
        if (!scroll_offset.has_value())
            continue;

        if (scrollbar_hit_rect(scrollbar, *scroll_offset).to_type<float>().contains(position))
            return i;
    }
    return {};
}

Optional<CompositorContextState::ViewportScrollbarDrag> CompositorContextState::begin_viewport_scrollbar_drag(Gfx::FloatPoint position)
{
    for (size_t i = 0; i < viewport_scrollbars.size(); ++i) {
        auto const& scrollbar = viewport_scrollbars[i];
        auto scroll_offset = async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id, cached_scroll_state_snapshot);
        if (!scroll_offset.has_value())
            continue;

        auto expanded = hovered_viewport_scrollbar_index == i || captured_viewport_scrollbar_index == i;
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

        captured_viewport_scrollbar_index = i;
        hovered_viewport_scrollbar_index = i;
        viewport_scrollbar_thumb_grab_position = thumb_grab_position;
        return ViewportScrollbarDrag { i, primary_position, thumb_grab_position };
    }

    return {};
}

Optional<CompositorContextState::ViewportScrollbarDrag> CompositorContextState::captured_viewport_scrollbar_drag(Gfx::FloatPoint position)
{
    if (!captured_viewport_scrollbar_index.has_value())
        return {};
    auto scrollbar_index = *captured_viewport_scrollbar_index;
    if (scrollbar_index >= viewport_scrollbars.size()) {
        captured_viewport_scrollbar_index.clear();
        return {};
    }
    auto const& scrollbar = viewport_scrollbars[scrollbar_index];
    auto primary_position = position.primary_offset_for_orientation(orientation_for_scrollbar(scrollbar));
    return ViewportScrollbarDrag { scrollbar_index, primary_position, viewport_scrollbar_thumb_grab_position };
}

Optional<CompositorContextState::ViewportScrollbarDrag> CompositorContextState::release_captured_viewport_scrollbar_drag(Gfx::FloatPoint position)
{
    if (!captured_viewport_scrollbar_index.has_value())
        return {};
    auto scrollbar_index = *captured_viewport_scrollbar_index;
    auto thumb_grab_position = viewport_scrollbar_thumb_grab_position;
    if (scrollbar_index >= viewport_scrollbars.size()) {
        captured_viewport_scrollbar_index.clear();
        return {};
    }
    auto const& scrollbar = viewport_scrollbars[scrollbar_index];
    auto primary_position = position.primary_offset_for_orientation(orientation_for_scrollbar(scrollbar));
    captured_viewport_scrollbar_index.clear();
    return ViewportScrollbarDrag { scrollbar_index, primary_position, thumb_grab_position };
}

bool CompositorContextState::set_hovered_viewport_scrollbar(Optional<size_t> scrollbar_index)
{
    if (hovered_viewport_scrollbar_index == scrollbar_index)
        return false;
    hovered_viewport_scrollbar_index = scrollbar_index;
    return true;
}

Optional<CompositorContextState::AppliedViewportScrollbarDrag> CompositorContextState::apply_viewport_scrollbar_drag(size_t scrollbar_index, float primary_position, float thumb_grab_position)
{
    if (scrollbar_index >= viewport_scrollbars.size())
        return {};

    auto const& scrollbar = viewport_scrollbars[scrollbar_index];
    auto expanded = hovered_viewport_scrollbar_index == scrollbar_index || captured_viewport_scrollbar_index == scrollbar_index;
    auto scroll_size = scrollbar_scroll_size(scrollbar, expanded);
    if (scroll_size == 0)
        return {};

    auto current_scroll_offset = async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id, cached_scroll_state_snapshot);
    if (!current_scroll_offset.has_value())
        return {};

    auto orientation = orientation_for_scrollbar(scrollbar);
    auto thumb_rect = expanded ? scrollbar.expanded_thumb_rect : scrollbar.thumb_rect;
    auto min_thumb_position = static_cast<float>(thumb_rect.primary_offset_for_orientation(orientation));
    auto max_thumb_position = min_thumb_position + scrollbar.max_scroll_offset * static_cast<float>(scroll_size);
    auto target_thumb_position = AK::clamp(primary_position - thumb_grab_position, min_thumb_position, max_thumb_position);
    auto target_scroll_offset = (target_thumb_position - min_thumb_position) / static_cast<float>(scroll_size);

    Gfx::FloatPoint delta;
    delta.set_primary_offset_for_orientation(orientation, target_scroll_offset - current_scroll_offset->primary_offset_for_orientation(orientation));
    if (delta.x() == 0 && delta.y() == 0)
        return {};

    auto scroll_offsets = async_scroll_tree.apply_scroll_delta(scrollbar.scroll_node_id, delta, cached_scroll_state_snapshot);
    if (scroll_offsets.is_empty())
        return {};
    async_scroll_tree.rebuild_wheel_hit_test_targets(cached_display_list, cached_scroll_state_snapshot);

    auto viewport_scroll_offset = viewport_scroll_offset_from(scroll_offsets);
    if (!viewport_scroll_offset.has_value())
        return {};

    return AppliedViewportScrollbarDrag { move(scroll_offsets), *viewport_scroll_offset };
}

CompositorContextState::ViewportScrollbarOverlayState CompositorContextState::viewport_scrollbar_overlay_state() const
{
    return {
        .scrollbars = viewport_scrollbars,
        .hovered_scrollbar_index = hovered_viewport_scrollbar_index,
        .captured_scrollbar_index = captured_viewport_scrollbar_index,
    };
}

void CompositorContextState::paint_viewport_scrollbar_overlay(Gfx::PaintingSurface& surface, ViewportScrollbarOverlayState const& overlay_state, Painting::ScrollStateSnapshot const& scroll_state_snapshot)
{
    paint_viewport_scrollbars(surface, overlay_state.scrollbars, scroll_state_snapshot, overlay_state.hovered_scrollbar_index, overlay_state.captured_scrollbar_index);
}

Optional<Gfx::FloatPoint> CompositorContextState::viewport_scroll_offset_from(Vector<AsyncScrollOffset> const& scroll_offsets)
{
    for (auto const& scroll_offset : scroll_offsets) {
        if (scroll_offset.stable_node_id.kind == AsyncScrollNodeKind::Viewport)
            return scroll_offset.compositor_scroll_offset;
    }
    return {};
}

}
