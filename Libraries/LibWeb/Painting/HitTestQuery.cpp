/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/HitTestQuery.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/ResizeHandle.h>

namespace Web::Painting {

Layout::RustFFI::FfiHitTestQuery HitTestQuery::ffi_query() const
{
    return {
        .device_pixels_per_css_pixel = m_device_pixels_per_css_pixel,
        .chrome_metrics = m_chrome_metrics,
        .has_visual_animation_sample_time = m_visual_animation_sample_time_nanoseconds.has_value(),
        .visual_animation_sample_time_ns = m_visual_animation_sample_time_nanoseconds.value_or(0),
    };
}

// https://html.spec.whatwg.org/multipage/image-maps.html#image-map-processing-model
// NB: The image publishes the areas of the map it is associated with onto its row, so the hit names one of them without
//     asking the DOM for the map or for the areas' attributes.
static DOM::NodeIdentity image_map_area_for_point(Layout::Node const& layout_node, CSSPixelPoint local_point)
{
    // For historical reasons, the coordinates must be interpreted relative to the displayed image after any stretching
    // caused by the CSS 'width' and 'height' properties.
    auto point = (local_point - Painting::absolute_rect(layout_node).location()).to_type<float>();
    auto area = Layout::RustFFI::layout_image_map_area_for_point(layout_node.document_host(), Layout::Node::slot_id(&layout_node), point.x(), point.y());
    if (area == 0)
        return {};
    return DOM::NodeIdentity::of_style_node(CSS::StyleNodeID { area });
}

HitTestResult HitTestQuery::hit_test_result_for(Layout::BegunRead const& read, Layout::RustFFI::FfiPointHit const& hit) const
{
    CSSPixelPoint local_point = hit.local;
    auto const* paintable_layout_node = layout_node_for_committed_slot(read, *m_arena, hit.paintable);
    auto hit_node = hit.hit_node;

    auto const* named_layout_node = layout_node_for_committed_slot(read, *m_arena, hit_node);
    VERIFY(named_layout_node && as<Layout::NodeWithStyle>(*named_layout_node).pointer_events() != CSS::PointerEvents::None);

    // https://drafts.csswg.org/cssom-view/#dom-document-elementfrompoint
    // 2. If there is a box in the viewport that would be a target for hit testing at coordinates x,y, when applying
    //    the transforms that apply to the descendants of the viewport, return the associated element and terminate
    //    these steps.
    // 3. If the document has a root element, return the root element and terminate these steps.
    // AD-HOC: Our viewport refers to the document instead of the root element. The steps above imply that we should
    //         not hit test the viewport as a box, and report the root element as hit when we otherwise miss, so we
    //         correct those hits here. This is where both pointer event hit testing and elementFromPoint() converge.
    GC::Ptr<DOM::Node> root_element;
    if (paintable_layout_node && paintable_layout_node->kind() == Layout::RustFFI::NodeKind::Viewport) {
        auto* named_element = const_cast<DOM::Element*>(paintable_layout_node->document().document_element());
        if (auto* root_layout_node = named_element ? named_element->unsafe_layout_node(read) : nullptr; root_layout_node && has_committed_box(*root_layout_node)) {
            root_element = named_element;
            hit_node = committed_row_slot(*root_layout_node);
        }
    }

    auto const& resolved = hit.resolved;
    auto identity = DOM::NodeIdentity::of(root_element.ptr());
    // NB: Style runs before layout, so a laid-out document's style engine tracks its tree and every connected element
    //     in it has a StyleNodeID.
    VERIFY(!root_element || identity);
    if (identity.is_none() && paintable_layout_node)
        identity = image_map_area_for_point(*paintable_layout_node, local_point);
    if (identity.is_none())
        identity = node_identity_of(resolved.dispatch);
    if (identity.is_none())
        identity = node_identity_of(resolved.fallback_dispatch);

    auto chrome_widget_kind = static_cast<ChromeWidgetKind>(hit.chrome_widget_kind);
    RefPtr<ChromeWidget> chrome_widget;
    if (chrome_widget_kind == ChromeWidgetKind::ResizeHandle)
        chrome_widget = m_chrome_widget_registry->get_or_create_resize_handle(*m_arena, hit.paintable);

    auto result = HitTestResult {
        .node = identity,
        .hit_node = hit_node,
        .arena = *m_arena,
        .chrome_widget = move(chrome_widget),
        .is_scrollbar = first_is_one_of(chrome_widget_kind, ChromeWidgetKind::HorizontalScrollbar, ChromeWidgetKind::VerticalScrollbar),
        .is_text_fragment = resolved.is_text_fragment,
    };
    if (resolved.has_index_in_node)
        result.index_in_node = resolved.index_in_node;
    return result;
}

Optional<CaretPosition> HitTestQuery::caret_position_from(Layout::RustFFI::FfiCaretAt const& caret_at) const
{
    auto const& resolved = caret_at.caret;
    if (!resolved.has_position)
        return {};
    auto* document = m_arena->document();
    if (!document)
        return {};
    auto dom_node = node_identity_of(resolved.node).resolve(*document);
    if (!dom_node)
        return {};

    Optional<CSSPixelRect> debug_rect;
    if (resolved.has_debug_rect)
        debug_rect = resolved.debug_rect;

    switch (resolved.boundary) {
    case Layout::RustFFI::FfiCaretBoundaryKind::Offset:
        return CaretPosition {
            .paintable = caret_at.paintable,
            .arena = *m_arena,
            .boundary = { DOM::NodeIdentity::of(*dom_node), static_cast<WebIDL::UnsignedLong>(resolved.offset) },
            .affinity = resolved.affinity_is_upstream ? TextAffinity::Upstream : TextAffinity::Downstream,
            .debug_rect = debug_rect,
        };
    case Layout::RustFFI::FfiCaretBoundaryKind::BeforeNode:
    case Layout::RustFFI::FfiCaretBoundaryKind::AfterNode:
    case Layout::RustFFI::FfiCaretBoundaryKind::IndexOfNodeInParent:
        break;
    }

    auto* parent = dom_node->parent();
    if (!parent)
        return {};
    auto parent_identity = DOM::NodeIdentity::of(*parent);
    if (resolved.boundary == Layout::RustFFI::FfiCaretBoundaryKind::IndexOfNodeInParent) {
        return CaretPosition {
            .paintable = caret_at.paintable,
            .arena = *m_arena,
            .boundary = { parent_identity, static_cast<WebIDL::UnsignedLong>(dom_node->index()) },
            .debug_rect = debug_rect,
        };
    }
    auto before_boundary = BoundaryIdentity { parent_identity, static_cast<WebIDL::UnsignedLong>(dom_node->index()) };
    auto after_boundary = BoundaryIdentity { parent_identity, static_cast<WebIDL::UnsignedLong>(dom_node->index() + 1) };
    auto is_before = resolved.boundary == Layout::RustFFI::FfiCaretBoundaryKind::BeforeNode;
    return CaretPosition {
        .paintable = caret_at.paintable,
        .arena = *m_arena,
        .boundary = is_before ? before_boundary : after_boundary,
        .secondary_boundary = is_before ? after_boundary : before_boundary,
        .debug_rect = debug_rect,
    };
}

Optional<CaretPosition> HitTestQuery::caret_position_from_point(Layout::BegunRead const& read, CSSPixelPoint point, GC::Ptr<DOM::Node const> constraint_scope) const
{
    auto const* scope_layout_node = constraint_scope ? constraint_scope->unsafe_layout_node(read) : nullptr;
    auto scope = scope_layout_node ? Layout::Node::slot_id(scope_layout_node) : Compositing::RustFFI::NodeSlotId { Compositing::RustFFI::INVALID_NODE_SLOT_INDEX };
    Layout::RustFFI::FfiCaretAt caret_at;
    Layout::RustFFI::layout_hit_test_caret_position_from_point(m_arena->host(), &read, ffi_query(), point, scope, &caret_at);
    return caret_position_from(caret_at);
}

Optional<HitTestResult> HitTestQuery::hit_test(Layout::BegunRead const& read, CSSPixelPoint point) const
{
    Layout::RustFFI::FfiPointHit hit;
    if (!Layout::RustFFI::layout_hit_test_topmost_hit(m_arena->host(), &read, ffi_query(), point, &hit))
        return {};
    return hit_test_result_for(read, hit);
}

TraversalDecision HitTestQuery::hit_test_all(Layout::BegunRead const& read, CSSPixelPoint point, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    Layout::RustFFI::FfiPointHit topmost_hit;
    if (!Layout::RustFFI::layout_hit_test_topmost_hit(m_arena->host(), &read, ffi_query(), point, &topmost_hit))
        return TraversalDecision::Continue;
    if (callback(hit_test_result_for(read, topmost_hit)) == TraversalDecision::Break)
        return TraversalDecision::Break;

    Vector<Layout::RustFFI::FfiPointHit> hits;
    Layout::RustFFI::layout_hit_test_all_hits(m_arena->host(), &read, ffi_query(), point, &hits, [](void* sink, Layout::RustFFI::FfiPointHit const* hit) {
        static_cast<Vector<Layout::RustFFI::FfiPointHit>*>(sink)->append(*hit);
    });
    for (size_t index_below_topmost_hit = 1; index_below_topmost_hit < hits.size(); ++index_below_topmost_hit) {
        if (callback(hit_test_result_for(read, hits[index_below_topmost_hit])) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    return TraversalDecision::Continue;
}

}
