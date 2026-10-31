/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/ResizeHandle.h>

namespace Web::Painting {

ChromeWidgetRegistry::ChromeWidgetRegistry() = default;

ChromeWidgetRegistry::~ChromeWidgetRegistry()
{
    clear();
}

RefPtr<ResizeHandle> ChromeWidgetRegistry::resize_handle(Compositing::RustFFI::NodeSlotId slot) const
{
    auto resize_handle = m_resize_handles.find(slot.index);
    if (resize_handle == m_resize_handles.end())
        return nullptr;
    return resize_handle->value;
}

NonnullRefPtr<ResizeHandle> ChromeWidgetRegistry::get_or_create_resize_handle(Layout::NodeArena& arena, Compositing::RustFFI::NodeSlotId slot)
{
    return m_resize_handles.ensure(slot.index, [&] { return ResizeHandle::create(arena, slot); });
}

void ChromeWidgetRegistry::drop_widgets_for_slot(Compositing::RustFFI::NodeSlotId slot)
{
    if (auto resize_handle = m_resize_handles.take(slot.index); resize_handle.has_value())
        resize_handle.value()->detach({});
}

void ChromeWidgetRegistry::clear()
{
    for (auto& entry : m_resize_handles)
        entry.value->detach({});
    m_resize_handles.clear();
}

ChromeWidget::ChromeWidget(Layout::NodeArena& arena, Compositing::RustFFI::NodeSlotId slot)
    : m_arena(arena)
    , m_slot(slot)
{
}

Layout::Node* ChromeWidget::layout_node() const
{
    Layout::ForcedReadScope read { m_arena->render_document() };
    return layout_node_for_committed_slot(read, *m_arena, m_slot);
}

void ChromeWidget::detach(Badge<ChromeWidgetRegistry>)
{
    did_detach();
    m_slot = Compositing::RustFFI::NodeSlotId_INVALID;
}

PhysicalResizeAxes physical_resize_axes(Layout::Node const& node)
{
    auto axes = Layout::RustFFI::layout_row_paintable_physical_resize_axes(node.document_host(), committed_row_slot(node));
    return { axes.horizontal, axes.vertical };
}

}
