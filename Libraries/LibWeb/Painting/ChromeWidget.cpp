/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/ResizeHandle.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/Painting/Scrollbar.h>

namespace Web::Painting {

ChromeWidgetRegistry::ChromeWidgetRegistry() = default;

ChromeWidgetRegistry::~ChromeWidgetRegistry()
{
    clear();
}

RefPtr<Scrollbar> ChromeWidgetRegistry::scrollbar(Layout::RustFFI::NodeSlotId slot, ScrollDirection direction) const
{
    auto entry = m_entries.find(slot.index);
    if (entry == m_entries.end())
        return nullptr;
    return direction == ScrollDirection::Horizontal ? entry->value.horizontal_scrollbar : entry->value.vertical_scrollbar;
}

NonnullRefPtr<Scrollbar> ChromeWidgetRegistry::get_or_create_scrollbar(Layout::NodeArena& arena, Layout::RustFFI::NodeSlotId slot, ScrollDirection direction)
{
    auto& entry = m_entries.ensure(slot.index);
    auto& scrollbar = direction == ScrollDirection::Horizontal ? entry.horizontal_scrollbar : entry.vertical_scrollbar;
    if (!scrollbar)
        scrollbar = Scrollbar::create(arena, slot, direction);
    return *scrollbar;
}

RefPtr<ResizeHandle> ChromeWidgetRegistry::resize_handle(Layout::RustFFI::NodeSlotId slot) const
{
    auto entry = m_entries.find(slot.index);
    if (entry == m_entries.end())
        return nullptr;
    return entry->value.resize_handle;
}

NonnullRefPtr<ResizeHandle> ChromeWidgetRegistry::get_or_create_resize_handle(Layout::NodeArena& arena, Layout::RustFFI::NodeSlotId slot)
{
    auto& entry = m_entries.ensure(slot.index);
    if (!entry.resize_handle)
        entry.resize_handle = ResizeHandle::create(arena, slot);
    return *entry.resize_handle;
}

void ChromeWidgetRegistry::drop_widgets_for_slot(Layout::RustFFI::NodeSlotId slot)
{
    auto entry = m_entries.take(slot.index);
    if (!entry.has_value())
        return;
    if (entry->horizontal_scrollbar)
        entry->horizontal_scrollbar->detach({});
    if (entry->vertical_scrollbar)
        entry->vertical_scrollbar->detach({});
    if (entry->resize_handle)
        entry->resize_handle->detach({});
}

void ChromeWidgetRegistry::clear()
{
    for (auto& entry : m_entries) {
        if (entry.value.horizontal_scrollbar)
            entry.value.horizontal_scrollbar->detach({});
        if (entry.value.vertical_scrollbar)
            entry.value.vertical_scrollbar->detach({});
        if (entry.value.resize_handle)
            entry.value.resize_handle->detach({});
    }
    m_entries.clear();
}

ChromeWidget::ChromeWidget(Layout::NodeArena& arena, Layout::RustFFI::NodeSlotId slot)
    : m_arena(arena)
    , m_slot(slot)
{
}

Layout::Node* ChromeWidget::layout_node() const
{
    return layout_node_for_committed_slot(*m_arena, m_slot);
}

void ChromeWidget::detach(Badge<ChromeWidgetRegistry>)
{
    did_detach();
    m_slot = Layout::RustFFI::NodeSlotId_INVALID;
}

PhysicalResizeAxes physical_resize_axes(Layout::Node const& node)
{
    auto axes = Layout::RustFFI::layout_arena_paintable_physical_resize_axes(node.arena_handle(), committed_row_slot(node));
    return { axes.horizontal, axes.vertical };
}

Optional<ScrollbarData> compute_scrollbar_data(Layout::Node const& node, ScrollDirection direction, ChromeMetrics const& metrics, ScrollStateSnapshot const* scroll_state_snapshot, ScrollbarSizing scrollbar_sizing)
{
    auto& document = node.document();
    auto overflow_x = overflow_value_applied_to_viewport_for_wheel_scrolling(document, ScrollDirection::Horizontal);
    auto overflow_y = overflow_value_applied_to_viewport_for_wheel_scrolling(document, ScrollDirection::Vertical);
    float device_scroll_offset = 0;
    if (scroll_state_snapshot) {
        auto own_offset = scroll_state_snapshot->device_offset_for_index(own_scroll_node_index(node));
        device_scroll_offset = direction == ScrollDirection::Horizontal ? -own_offset.x() : -own_offset.y();
    }
    auto result = Layout::RustFFI::layout_arena_paintable_compute_scrollbar_data(
        node.arena_handle(), committed_row_slot(node), static_cast<Layout::RustFFI::ScrollDirection>(direction),
        metrics, to_underlying(overflow_x), to_underlying(overflow_y), scrollbar_sizing == ScrollbarSizing::Enlarged,
        scroll_state_snapshot, device_scroll_offset, document.page().client().device_pixels_per_css_pixel());
    if (!result.has_value)
        return {};
    return ScrollbarData {
        .gutter_rect = result.value.gutter_rect,
        .thumb_rect = result.value.thumb_rect,
        .track_rect = result.value.track_rect,
        .thumb_travel_to_scroll_ratio = result.value.thumb_travel_to_scroll_ratio,
    };
}

}
