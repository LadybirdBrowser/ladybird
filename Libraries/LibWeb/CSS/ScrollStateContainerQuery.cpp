/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Invalidation/ContainerQueryInvalidator.h>
#include <LibWeb/CSS/RustQueryHandle.h>
#include <LibWeb/CSS/ScrollStateContainerQuery.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/ScrollSnap.h>
#include <LibWeb/Painting/Scrolling.h>

namespace Web::CSS {

using namespace Parser::ValueParserFFI;

static bool is_scroll_state_container(DOM::Element const& element)
{
    auto const* box_values = element.style_group<ComputedValues::BoxValues>();
    return box_values && box_values->is_scroll_state_container;
}

// The scrolling box a scroll-state(scrollable) or scroll-state(scrolled) query asks about. The root element's is the
// viewport's.
static Layout::Node* scrolling_box_of(DOM::Document& document, DOM::Element& element)
{
    if (&element == document.document_element())
        return document.unsafe_layout_node();
    auto* layout_node = element.unsafe_layout_node();
    if (!layout_node || !layout_node->is_scroll_container())
        return nullptr;
    return layout_node;
}

// https://drafts.csswg.org/css-conditional-5/#stuck
static u8 stuck_edges(DOM::Document& document, Layout::Node const& layout_node)
{
    auto const* node_with_style = as_if<Layout::NodeWithStyle>(layout_node);
    if (!node_with_style || !node_with_style->is_sticky_position())
        return 0;
    auto sticky_node_index = Layout::RustFFI::layout_arena_sticky_spatial_node_index(layout_node.arena_handle(), Painting::committed_row_slot(layout_node));
    if (sticky_node_index == NumericLimits<u32>::max())
        return 0;

    // A sticky box is stuck to the edge whose inset moved it from its normal position.
    auto offset = document.paint_state().scroll_state_snapshot().device_offset_for_index(Painting::SpatialNodeIndex { sticky_node_index });
    u8 edges = 0;
    if (offset.y() > 0)
        edges |= SCROLL_STATE_EDGE_TOP;
    else if (offset.y() < 0)
        edges |= SCROLL_STATE_EDGE_BOTTOM;
    if (offset.x() > 0)
        edges |= SCROLL_STATE_EDGE_LEFT;
    else if (offset.x() < 0)
        edges |= SCROLL_STATE_EDGE_RIGHT;
    return edges;
}

// https://drafts.csswg.org/css-conditional-5/#scrollable
static u8 scrollable_edges(Layout::Node const& scrolling_box)
{
    if (!Painting::has_committed_box(scrolling_box) || !Painting::scrollable_overflow_rect(scrolling_box).has_value())
        return 0;

    // Only the directions the user can scroll in count. A box with hidden overflow in an axis can still be scrolled
    // programmatically in it, so its scroll range alone does not say that.
    auto user_scrollable_axes = Painting::wheel_scrollable_axes(scrolling_box);

    auto offset = Painting::scroll_offset(scrolling_box);
    auto minimum_offset = Painting::minimum_scroll_offset(scrolling_box);
    auto maximum_offset = Painting::maximum_scroll_offset(scrolling_box);
    u8 edges = 0;
    if (user_scrollable_axes.vertical) {
        if (offset.y() > minimum_offset.y())
            edges |= SCROLL_STATE_EDGE_TOP;
        if (offset.y() < maximum_offset.y())
            edges |= SCROLL_STATE_EDGE_BOTTOM;
    }
    if (user_scrollable_axes.horizontal) {
        if (offset.x() > minimum_offset.x())
            edges |= SCROLL_STATE_EDGE_LEFT;
        if (offset.x() < maximum_offset.x())
            edges |= SCROLL_STATE_EDGE_RIGHT;
    }
    return edges;
}

// https://drafts.csswg.org/css-conditional-5/#snapped
static u8 snapped_axes(DOM::Document& document, DOM::Element& element, Layout::Node const& layout_node)
{
    // A snap area is snapped by its nearest ancestor scroll container, if that is a snap container.
    Layout::Box const* snap_container = layout_node.containing_block();
    while (snap_container && !snap_container->is_scroll_container())
        snap_container = snap_container->containing_block();
    if (!snap_container || !Painting::is_scroll_snap_container(*snap_container))
        return 0;

    auto stable_node_id = Painting::async_scroll_node_stable_id(*snap_container);
    if (!stable_node_id.has_value())
        return 0;

    auto const& snapped_areas = document.snapped_areas_of_scroll_container(*stable_node_id);
    Painting::SnapAreaIdentity identity { .node_id = element.unique_id(), .pseudo_element_type = 0 };
    u8 axes = 0;
    if (snapped_areas.x.contains_slow(identity))
        axes |= SCROLL_STATE_SNAPPED_X;
    if (snapped_areas.y.contains_slow(identity))
        axes |= SCROLL_STATE_SNAPPED_Y;
    return axes;
}

ScrollStateSnapshot ScrollStateQueryContainers::snapshot_for_query(DOM::Element& container)
{
    return m_containers.ensure(container).snapshot;
}

void ScrollStateQueryContainers::did_scroll_relatively(Layout::Node const& scrolling_box, CSSPixelPoint delta)
{
    DOM::Element* element = nullptr;
    if (!scrolling_box.is_viewport()) {
        if (scrolling_box.generated_for_pseudo_element().has_value())
            return;
        element = const_cast<DOM::Element*>(as_if<DOM::Element>(scrolling_box.dom_node()));
        if (!element)
            return;
    }

    // https://drafts.csswg.org/css-conditional-5/#scrolled
    // The most recent scroll in each axis decides that axis, and a scroll in one axis keeps the other's.
    u8 direction = element ? element->last_relative_scroll_direction() : m_viewport_last_relative_scroll_direction;
    if (delta.x() != 0) {
        direction &= ~(SCROLL_STATE_EDGE_LEFT | SCROLL_STATE_EDGE_RIGHT);
        direction |= delta.x() > 0 ? SCROLL_STATE_EDGE_RIGHT : SCROLL_STATE_EDGE_LEFT;
    }
    if (delta.y() != 0) {
        direction &= ~(SCROLL_STATE_EDGE_TOP | SCROLL_STATE_EDGE_BOTTOM);
        direction |= delta.y() > 0 ? SCROLL_STATE_EDGE_BOTTOM : SCROLL_STATE_EDGE_TOP;
    }
    if (element)
        element->set_last_relative_scroll_direction(direction);
    else
        m_viewport_last_relative_scroll_direction = direction;
}

bool ScrollStateQueryContainers::snapshot_post_layout_state(DOM::Document& document, Snapshot which)
{
    if (m_containers.is_empty())
        return false;
    if (which == Snapshot::NewContainersOnly && all_of(m_containers, [](auto const& it) { return it.value.has_been_snapshotted; }))
        return false;

    // Sticky offsets are resolved with the scroll state of the settled visual context tree.
    document.update_paint_and_hit_testing_properties_if_needed();

    Vector<GC::Ref<DOM::Element>> containers_to_forget;
    bool any_state_changed = false;
    for (auto& [element, container] : m_containers) {
        if (which == Snapshot::NewContainersOnly && container.has_been_snapshotted)
            continue;
        container.has_been_snapshotted = true;
        if (!element->is_connected() || &element->document() != &document || !is_scroll_state_container(element)) {
            containers_to_forget.append(element);
            continue;
        }

        ScrollStateSnapshot snapshot;
        if (auto* layout_node = element->unsafe_layout_node(); layout_node && Painting::has_committed_box(*layout_node)) {
            snapshot.stuck = stuck_edges(document, *layout_node);
            snapshot.snapped = snapped_axes(document, element, *layout_node);
        }
        if (auto* scrolling_box = scrolling_box_of(document, element)) {
            snapshot.scrollable = scrollable_edges(*scrolling_box);
            snapshot.scrolled = scrolling_box->is_viewport() ? m_viewport_last_relative_scroll_direction : element->last_relative_scroll_direction();
        }

        if (snapshot == container.snapshot)
            continue;
        container.snapshot = snapshot;
        any_state_changed = true;
        Invalidation::invalidate_descendant_styles_depending_on_size_container_query(element);
    }

    for (auto& element : containers_to_forget) {
        auto container = m_containers.take(element);
        // A container that goes away with state still has styles that read it, and they read no state now.
        if (container.has_value() && container->snapshot != ScrollStateSnapshot {} && element->is_connected()) {
            any_state_changed = true;
            Invalidation::invalidate_descendant_styles_depending_on_size_container_query(element);
        }
    }
    return any_state_changed;
}

void ScrollStateQueryContainers::visit_edges(GC::Cell::Visitor& visitor)
{
    for (auto& it : m_containers)
        visitor.visit(it.key);
}

}
