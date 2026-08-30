/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/ResizeHandle.h>
#include <LibWeb/Painting/Scrollbar.h>
#include <LibWeb/Painting/Scrolling.h>

namespace Web::Painting {

NonnullRefPtr<HitTestDisplayList> HitTestDisplayList::create_from_rust_recording(u64 visual_context_tree_version, Layout::NodeArena& arena, ChromeWidgetRegistry& chrome_widget_registry)
{
    auto* arena_handle = arena.handle();
    auto list = adopt_ref(*new HitTestDisplayList(visual_context_tree_version, arena, chrome_widget_registry, Layout::RustFFI::layout_arena_hit_test_list_generation(arena_handle)));
    struct VisitContext {
        HitTestDisplayList& list;
        Layout::NodeArena& arena;
        ChromeWidgetRegistry& chrome_widget_registry;
    };
    VisitContext visit_context { *list, arena, chrome_widget_registry };
    Layout::RustFFI::layout_arena_hit_test_visit_caret_roots_and_chrome_widgets(arena_handle, &visit_context,
        [](void* sink, Layout::RustFFI::NodeSlotId paintable, u8 chrome_widget_kind, void* caret_node_shell) {
            auto& context = *static_cast<VisitContext*>(sink);
            if (caret_node_shell) {
                if (auto* caret_node = static_cast<Layout::Node*>(caret_node_shell)->dom_node())
                    context.list.m_caret_node_roots.append(caret_node);
            }
            switch (static_cast<ChromeWidgetKind>(chrome_widget_kind)) {
            case ChromeWidgetKind::None:
                break;
            case ChromeWidgetKind::ResizeHandle:
                (void)context.chrome_widget_registry.get_or_create_resize_handle(context.arena, paintable);
                break;
            case ChromeWidgetKind::HorizontalScrollbar:
                (void)context.chrome_widget_registry.get_or_create_scrollbar(context.arena, paintable, ScrollDirection::Horizontal);
                break;
            case ChromeWidgetKind::VerticalScrollbar:
                (void)context.chrome_widget_registry.get_or_create_scrollbar(context.arena, paintable, ScrollDirection::Vertical);
                break;
            }
        });
    return list;
}

HitTestDisplayList::HitTestDisplayList(u64 visual_context_tree_version, Layout::NodeArena& arena, ChromeWidgetRegistry& chrome_widget_registry, u64 rust_generation)
    : m_visual_context_tree_version(visual_context_tree_version)
    , m_arena(arena)
    , m_chrome_widget_registry(chrome_widget_registry)
    , m_rust_generation(rust_generation)
{
}

void HitTestDisplayList::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_caret_node_roots);
}

bool HitTestDisplayList::is_current() const
{
    return m_rust_generation != 0 && Layout::RustFFI::layout_arena_hit_test_list_generation(m_arena->handle()) == m_rust_generation;
}

HitTestDisplayList::Item HitTestDisplayList::item(size_t index) const
{
    return { index, Layout::RustFFI::layout_arena_hit_test_item_facts(m_arena->handle(), index) };
}

static DOM::Node const* dom_node_for_shell(void* shell)
{
    auto const* layout_node = static_cast<Layout::Node const*>(shell);
    return layout_node ? layout_node->dom_node() : nullptr;
}

static Optional<Gfx::FloatPoint> local_float_point_for_visual_context(ContextRef context, CSSPixelPoint point, DOM::Document const& document, double device_pixels_per_css_pixel, AccumulatedVisualContextTree::ClipBehavior clip_behavior)
{
    auto pixel_ratio = static_cast<float>(device_pixels_per_css_pixel);
    auto const& visual_context_tree = document.visual_context_tree();
    auto result = visual_context_tree.transform_point_for_hit_test(context, point.to_type<float>() * pixel_ratio, document.scroll_state_snapshot(), clip_behavior);
    if (!result.has_value())
        return {};
    return *result / pixel_ratio;
}

Optional<CSSPixelPoint> HitTestDisplayList::local_point_for_visual_context(ContextRef context, CSSPixelPoint point, DOM::Document const& document, double device_pixels_per_css_pixel) const
{
    return local_float_point_for_visual_context(context, point, document, device_pixels_per_css_pixel, AccumulatedVisualContextTree::ClipBehavior::Respect)
        .map([](auto float_point) { return float_point.template to_type<CSSPixels>(); });
}

CSSPixelRect HitTestDisplayList::viewport_rect_for_context(SpatialNodeIndex spatial, CSSPixelRect const& rect, DOM::Document const& document, double device_pixels_per_css_pixel) const
{
    auto pixel_ratio = static_cast<float>(device_pixels_per_css_pixel);
    auto const& visual_context_tree = document.visual_context_tree();
    auto result = visual_context_tree.transform_rect_to_viewport(spatial, rect.to_type<float>() * pixel_ratio, document.scroll_state_snapshot());
    return result.scaled(1.0f / pixel_ratio).to_type<CSSPixels>();
}

struct HitTestDisplayList::QueryContext {
    GC::Ptr<DOM::Document const> document;
    double device_pixels_per_css_pixel { 1 };
    ChromeMetrics const* chrome_metrics { nullptr };
    GC::Ptr<DOM::Node const> scope { nullptr };

    Layout::RustFFI::FfiHitTestQueryCallbacks callbacks()
    {
        auto scroll_offsets = document ? document->scroll_state_snapshot().device_offsets() : ReadonlySpan<Gfx::FloatPoint> {};
        Layout::RustFFI::FfiHitTestQueryCallbacks callbacks {
            .context = this,
            .device_pixels_per_css_pixel = device_pixels_per_css_pixel,
            .scroll_offsets = scroll_offsets.data(),
            .scroll_offsets_len = scroll_offsets.size(),
            .has_chrome_metrics = chrome_metrics != nullptr,
            .chrome_metrics = {},
            .viewport_wheel_overflow_x = 0,
            .viewport_wheel_overflow_y = 0,
            .local_point_for_visual_context = [](void* context_pointer, ContextRef visual_context, CSSPixelPoint point, bool respect_clip, Gfx::FloatPoint* out) -> bool {
                auto& context = *static_cast<QueryContext*>(context_pointer);
                VERIFY(context.document);
                auto clip_behavior = respect_clip ? AccumulatedVisualContextTree::ClipBehavior::Respect : AccumulatedVisualContextTree::ClipBehavior::Ignore;
                auto local_point = local_float_point_for_visual_context(visual_context, point, *context.document, context.device_pixels_per_css_pixel, clip_behavior);
                if (!local_point.has_value())
                    return false;
                *out = *local_point;
                return true;
            },
            .shell_in_scope = [](void* context_pointer, void* shell) -> bool {
                auto& context = *static_cast<QueryContext*>(context_pointer);
                VERIFY(context.scope);
                auto const* dom_node = dom_node_for_shell(shell);
                return dom_node && context.scope->is_inclusive_ancestor_of(*dom_node);
            },
        };
        if (chrome_metrics)
            callbacks.chrome_metrics = *chrome_metrics;
        if (document) {
            callbacks.viewport_wheel_overflow_x = to_underlying(overflow_value_applied_to_viewport_for_wheel_scrolling(*document, ScrollDirection::Horizontal));
            callbacks.viewport_wheel_overflow_y = to_underlying(overflow_value_applied_to_viewport_for_wheel_scrolling(*document, ScrollDirection::Vertical));
        }
        return callbacks;
    }
};

struct CaretPositionQueryContext {
    GC::Ref<DOM::Node const> node;
    size_t offset { 0 };

    Layout::RustFFI::FfiCaretPositionQueryCallbacks callbacks()
    {
        return {
            .context = this,
            .shell_is_query_node = [](void* context_pointer, void* shell) -> bool {
                auto& context = *static_cast<CaretPositionQueryContext*>(context_pointer);
                return dom_node_for_shell(shell) == context.node.ptr();
            },
            .query_boundary_descends_to_shell = [](void* context_pointer, void* shell) -> bool {
                auto& context = *static_cast<CaretPositionQueryContext*>(context_pointer);
                auto const* shell_dom_node = dom_node_for_shell(shell);
                if (!shell_dom_node)
                    return false;
                auto* node_after_boundary = context.node->child_at_index(context.offset);
                while (node_after_boundary && node_after_boundary != shell_dom_node)
                    node_after_boundary = node_after_boundary->first_child();
                return node_after_boundary == shell_dom_node;
            },
            .query_boundary_follows_shell_end = [](void* context_pointer, void* shell, size_t end_offset_with_whitespace) -> bool {
                auto& context = *static_cast<CaretPositionQueryContext*>(context_pointer);
                auto const* shell_dom_node = dom_node_for_shell(shell);
                return shell_dom_node
                    && context.offset > 0
                    && context.node->child_at_index(context.offset - 1) == shell_dom_node
                    && end_offset_with_whitespace == shell_dom_node->length();
            },
            .query_is_adjacent_to_shell = [](void* context_pointer, void* shell) -> bool {
                auto& context = *static_cast<CaretPositionQueryContext*>(context_pointer);
                auto const* shell_dom_node = dom_node_for_shell(shell);
                return shell_dom_node
                    && shell_dom_node->parent() == context.node.ptr()
                    && (context.offset == shell_dom_node->index() || context.offset == shell_dom_node->index() + 1);
            },
        };
    }
};

Optional<HitTestDisplayList::TopmostItem> HitTestDisplayList::topmost_item_from(Layout::RustFFI::FfiTopmostItem const& item)
{
    if (!item.has_item)
        return {};
    return TopmostItem { item.index, item.local };
}

Optional<HitTestDisplayList::TopmostItem> HitTestDisplayList::find_topmost_item(CSSPixelPoint point, DOM::Document const& document, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics) const
{
    QueryContext context { &document, device_pixels_per_css_pixel, &chrome_metrics, nullptr };
    return topmost_item_from(Layout::RustFFI::layout_arena_hit_test_find_topmost_item(m_arena->handle(), context.callbacks(), point));
}

void HitTestDisplayList::find_topmost_items_for_caret(CSSPixelPoint point, DOM::Document const& document, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics, Optional<TopmostItem>& caret_item, Optional<TopmostItem>& hit_item) const
{
    QueryContext context { &document, device_pixels_per_css_pixel, &chrome_metrics, nullptr };
    auto items = Layout::RustFFI::layout_arena_hit_test_find_topmost_items_for_caret(m_arena->handle(), context.callbacks(), point);
    caret_item = topmost_item_from(items.caret_item);
    hit_item = topmost_item_from(items.hit_item);
}

Vector<size_t> HitTestDisplayList::hit_item_indices_topmost_first(CSSPixelPoint point, DOM::Document const& document, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics) const
{
    QueryContext context { &document, device_pixels_per_css_pixel, &chrome_metrics, nullptr };
    Vector<size_t> indices;
    Layout::RustFFI::layout_arena_hit_test_all(m_arena->handle(), context.callbacks(), point, &indices, [](void* sink, size_t index) {
        static_cast<Vector<size_t>*>(sink)->append(index);
    });
    return indices;
}

size_t HitTestDisplayList::item_index_at_line_edge(size_t line_index, CaretPositionType type) const
{
    return Layout::RustFFI::layout_arena_hit_test_item_at_line_edge(m_arena->handle(), line_index, to_underlying(type));
}

Optional<HitTestDisplayList::CaretItemForLine> HitTestDisplayList::caret_item_for_line(size_t line_index, CSSPixelPoint local_point, CaretPositionMode mode) const
{
    auto result = Layout::RustFFI::layout_arena_hit_test_caret_item_for_line(m_arena->handle(), line_index, local_point, to_underlying(mode));
    if (!result.has_item)
        return {};
    return CaretItemForLine { result.item_index, static_cast<CaretPositionType>(result.position_type) };
}

bool HitTestDisplayList::item_is_inline_adjacent_to_line(size_t item_index, size_t line_index) const
{
    return Layout::RustFFI::layout_arena_hit_test_item_is_inline_adjacent_to_line(m_arena->handle(), item_index, line_index);
}

HitTestDisplayList::ClosestLine HitTestDisplayList::find_closest_line(CSSPixelPoint point, DOM::Document const& document, double device_pixels_per_css_pixel, CaretPositionMode mode, DOM::Node const* scope_dom_node, AccumulatedVisualContextTree::ClipBehavior clip_behavior) const
{
    QueryContext context { &document, device_pixels_per_css_pixel, nullptr, scope_dom_node };
    auto result = Layout::RustFFI::layout_arena_hit_test_find_closest_line(m_arena->handle(), context.callbacks(), point, to_underlying(mode), scope_dom_node != nullptr, clip_behavior == AccumulatedVisualContextTree::ClipBehavior::Respect);
    ClosestLine closest_line;
    if (result.has_index)
        closest_line.index = result.index;
    closest_line.local_point = { CSSPixels::from_raw(result.local_x), CSSPixels::from_raw(result.local_y) };
    closest_line.block_distance = CSSPixels::from_raw(result.block_distance);
    return closest_line;
}

Layout::Node const* HitTestDisplayList::layout_node_for_item(Item item) const
{
    return layout_node_for_committed_slot(*m_arena, item.paintable());
}

RefPtr<ChromeWidget> HitTestDisplayList::chrome_widget_for_item(Item item) const
{
    switch (item.chrome_widget_kind()) {
    case ChromeWidgetKind::None:
        return nullptr;
    case ChromeWidgetKind::ResizeHandle:
        return m_chrome_widget_registry->resize_handle(item.paintable());
    case ChromeWidgetKind::HorizontalScrollbar:
        return m_chrome_widget_registry->scrollbar(item.paintable(), ScrollDirection::Horizontal);
    case ChromeWidgetKind::VerticalScrollbar:
        return m_chrome_widget_registry->scrollbar(item.paintable(), ScrollDirection::Vertical);
    }
    VERIFY_NOT_REACHED();
}

DOM::Node const* HitTestDisplayList::item_dom_node(size_t item_index) const
{
    return dom_node_for_shell(Layout::RustFFI::layout_arena_hit_test_item_target_shell(m_arena->handle(), item_index));
}

static DOM::Node const* dom_node_for_dispatch_shell(void* shell, bool allow_pseudo_fallback)
{
    if (auto const* node = dom_node_for_shell(shell))
        return node;
    auto const* layout_node = static_cast<Layout::Node const*>(shell);
    if (allow_pseudo_fallback && layout_node && layout_node->is_generated_for_pseudo_element())
        return layout_node->pseudo_element_generator().ptr();
    return nullptr;
}

DOM::Node const* HitTestDisplayList::event_dispatch_dom_node_for_item(size_t item_index) const
{
    bool allow_pseudo_fallback = false;
    auto* shell = Layout::RustFFI::layout_arena_hit_test_item_dispatch_shell(m_arena->handle(), item_index, &allow_pseudo_fallback);
    return dom_node_for_dispatch_shell(shell, allow_pseudo_fallback);
}

bool HitTestDisplayList::item_is_direct_caret_target(size_t item_index) const
{
    auto const* dom_node = item_dom_node(item_index);
    return dom_node && dom_node == event_dispatch_dom_node_for_item(item_index);
}

// https://html.spec.whatwg.org/multipage/image-maps.html#image-map-processing-model
static GC::Ptr<DOM::Node> image_map_area_for_point(Layout::Node const& layout_node, CSSPixelPoint local_point)
{
    auto* image_element = as_if<HTML::HTMLImageElement>(const_cast<DOM::Node*>(layout_node.dom_node()));
    if (!image_element)
        return {};

    auto map_element = image_element->associated_map_element();
    if (!map_element)
        return {};

    // For historical reasons, the coordinates must be interpreted relative to the displayed image after any stretching
    // caused by the CSS 'width' and 'height' properties.
    auto image_rect = Painting::absolute_rect(layout_node);
    return map_element->area_for_point(local_point - image_rect.location(), image_rect.size());
}

HitTestResult HitTestDisplayList::hit_test_result_for_item(Item item, CSSPixelPoint local_point) const
{
    auto const* paintable_layout_node = layout_node_for_item(item);
    auto hit_node = item.hit_node();

    auto const* named_layout_node = layout_node_for_committed_slot(*m_arena, hit_node);
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
        if (auto* root_layout_node = named_element ? named_element->unsafe_layout_node() : nullptr; root_layout_node && has_committed_box(*root_layout_node)) {
            root_element = named_element;
            hit_node = committed_row_slot(*root_layout_node);
        }
    }

    auto resolved = Layout::RustFFI::layout_arena_hit_test_resolve_hit(m_arena->handle(), item.index(), local_point);
    GC::Ptr<DOM::Node> node = root_element;
    if (!node && paintable_layout_node)
        node = image_map_area_for_point(*paintable_layout_node, local_point);
    if (!node)
        node = const_cast<DOM::Node*>(dom_node_for_dispatch_shell(resolved.dispatch_shell, resolved.allow_pseudo_fallback));
    if (!node)
        node = const_cast<DOM::Node*>(dom_node_for_dispatch_shell(resolved.fallback_dispatch_shell, false));

    // NB: Empty-line items are not reachable through regular hit testing; the descriptor still resolves them for
    // callers that already hold such an item.
    auto result = HitTestResult {
        .node = node,
        .hit_node = hit_node,
        .arena = *m_arena,
        .chrome_widget = chrome_widget_for_item(item),
        .is_text_fragment = resolved.is_text_fragment,
    };
    if (resolved.has_index_in_node)
        result.index_in_node = resolved.index_in_node;
    return result;
}

Optional<CaretPosition> HitTestDisplayList::caret_position_for_item(Item item, CSSPixelPoint local_point, CaretPositionType type) const
{
    auto resolved = Layout::RustFFI::layout_arena_hit_test_resolve_caret(m_arena->handle(), item.index(), local_point, to_underlying(type));
    if (!resolved.has_position || !resolved.node_shell)
        return {};
    auto* dom_node = static_cast<Layout::Node*>(resolved.node_shell)->dom_node();
    if (!dom_node)
        return {};

    Optional<CSSPixelRect> debug_rect;
    if (resolved.has_debug_rect)
        debug_rect = resolved.debug_rect;

    switch (resolved.boundary) {
    case Layout::RustFFI::FfiCaretBoundaryKind::Offset:
        return CaretPosition {
            .paintable = item.paintable(),
            .arena = *m_arena,
            .boundary = { *dom_node, static_cast<WebIDL::UnsignedLong>(resolved.offset) },
            .affinity = resolved.affinity_is_upstream ? TextAffinity::Upstream : TextAffinity::Downstream,
            .debug_rect = debug_rect,
        };
    case Layout::RustFFI::FfiCaretBoundaryKind::BeforeNode:
    case Layout::RustFFI::FfiCaretBoundaryKind::AfterNode:
        break;
    }

    auto* parent = dom_node->parent();
    if (!parent)
        return {};
    auto before_boundary = DOM::BoundaryPoint { *parent, static_cast<WebIDL::UnsignedLong>(dom_node->index()) };
    auto after_boundary = DOM::BoundaryPoint { *parent, static_cast<WebIDL::UnsignedLong>(dom_node->index() + 1) };
    auto is_before = resolved.boundary == Layout::RustFFI::FfiCaretBoundaryKind::BeforeNode;
    return CaretPosition {
        .paintable = item.paintable(),
        .arena = *m_arena,
        .boundary = is_before ? before_boundary : after_boundary,
        .secondary_boundary = is_before ? after_boundary : before_boundary,
        .debug_rect = debug_rect,
    };
}

Optional<CaretPosition> HitTestDisplayList::caret_position_for_hit_container(Item item) const
{
    auto dom_node = item_dom_node(item.index());
    if (!dom_node)
        return {};

    return CaretPosition {
        .paintable = item.paintable(),
        .arena = *m_arena,
        .boundary = { const_cast<DOM::Node&>(*dom_node), 0 },
        .debug_rect = item.caret_rect(),
    };
}

Optional<CaretPosition> HitTestDisplayList::caret_position_at_line_edge(DOM::Node const& node, size_t offset, TextAffinity affinity, CaretLineEdge edge) const
{
    if (!is_current())
        return {};
    CaretPositionQueryContext context { node, offset };
    auto line = Layout::RustFFI::layout_arena_hit_test_caret_line_for_position(m_arena->handle(), context.callbacks(), offset, affinity == TextAffinity::Downstream);
    if (!line.has_line)
        return {};
    auto type = edge == CaretLineEdge::Start ? CaretPositionType::Before : CaretPositionType::After;
    return caret_position_for_item(item(item_index_at_line_edge(line.line_index, type)), {}, type);
}

Optional<CaretPosition> HitTestDisplayList::caret_position_on_adjacent_line(DOM::Node const& node, size_t offset, TextAffinity affinity, CaretLineDirection direction, CSSPixels inline_coordinate, DOM::Node const& scope) const
{
    if (!is_current())
        return {};
    CaretPositionQueryContext position_context { node, offset };
    auto current_line = Layout::RustFFI::layout_arena_hit_test_caret_line_for_position(m_arena->handle(), position_context.callbacks(), offset, affinity == TextAffinity::Downstream);
    if (!current_line.has_line)
        return {};

    // INTEROP: Vertical caret movement in Chromium, WebKit, and Gecko follows rendered line geometry rather than DOM
    QueryContext context { nullptr, 1, nullptr, &scope };
    auto adjacent = Layout::RustFFI::layout_arena_hit_test_adjacent_line(m_arena->handle(), context.callbacks(), current_line.line_index, direction == CaretLineDirection::Next ? 1 : 0, inline_coordinate.raw_value());
    if (!adjacent.has_line)
        return {};
    // Reuse point-to-caret resolution after choosing the line so text, atomic boxes, and empty lines share one rule for
    // selecting the position closest to the preferred inline coordinate.
    return caret_position_for_line(adjacent.line_index, { CSSPixels::from_raw(adjacent.point_x), CSSPixels::from_raw(adjacent.point_y) }, CaretPositionMode::Normal);
}

Optional<CSSPixels> HitTestDisplayList::caret_line_block_coordinate(DOM::Node const& node, size_t offset, TextAffinity affinity) const
{
    if (!is_current())
        return {};
    CaretPositionQueryContext context { node, offset };
    auto line = Layout::RustFFI::layout_arena_hit_test_caret_line_for_position(m_arena->handle(), context.callbacks(), offset, affinity == TextAffinity::Downstream);
    if (!line.has_line)
        return {};
    return CSSPixels::from_raw(Layout::RustFFI::layout_arena_hit_test_line_block_coordinate(m_arena->handle(), line.line_index));
}

Optional<CaretPosition> HitTestDisplayList::caret_position_for_line(size_t line_index, CSSPixelPoint local_point, CaretPositionMode mode) const
{
    auto caret_item = caret_item_for_line(line_index, local_point, mode);
    if (!caret_item.has_value())
        return {};
    return caret_position_for_item(item(caret_item->item_index), local_point, caret_item->type);
}

Optional<CaretPosition> HitTestDisplayList::caret_position_from_point(CSSPixelPoint point, DOM::Document const& document, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics, CaretPositionMode mode, GC::Ptr<DOM::Node const> constraint_scope) const
{
    if (m_visual_context_tree_version != document.visual_context_tree().version() || !is_current())
        return {};
    // First find both the topmost hit-test item and the topmost item that can directly produce a caret.
    // Non-caret items are still needed to keep later line fallback scoped to the hit content.
    // FIXME: Caret placement compares items by record order alone, ignoring the depth-sorted paint order of
    //        planes inside 3D rendering contexts.
    Optional<TopmostItem> topmost_item;
    Optional<TopmostItem> topmost_hit_item;
    find_topmost_items_for_caret(point, document, device_pixels_per_css_pixel, chrome_metrics, topmost_item, topmost_hit_item);

    Optional<Item> topmost_hit_facts;
    if (topmost_hit_item.has_value())
        topmost_hit_facts = item(topmost_hit_item->index);

    // A constrained search only accepts direct hits inside the constraint scope.
    if (constraint_scope && topmost_item.has_value()) {
        auto const* item_node = item_dom_node(topmost_item->index);
        if (!item_node || !constraint_scope->is_inclusive_ancestor_of(*item_node))
            topmost_item = {};
    }

    // Direct caret hits win unless another non-caret item is visibly on top of them.
    auto topmost_caret_item_matches_hit_item = [&] {
        return topmost_hit_item.has_value()
            && topmost_item->index == topmost_hit_item->index
            && item_is_direct_caret_target(topmost_item->index);
    };
    if (topmost_item.has_value() && (constraint_scope || !topmost_hit_item.has_value() || topmost_caret_item_matches_hit_item())) {
        auto item_facts = item(topmost_item->index);
        if (auto caret_position = caret_position_for_item(item_facts, topmost_item->local_point); caret_position.has_value()) {
            if (caret_position->debug_rect.has_value())
                caret_position->debug_rect = viewport_rect_for_context(item_facts.context().spatial, *caret_position->debug_rect, document, device_pixels_per_css_pixel);
            return caret_position;
        }
    }

    // If the point is over a non-caret item, only consider caret lines inside that item's event-dispatch node first.
    // This prevents overlays or side content from snapping the caret to unrelated nearby text.
    DOM::Node const* line_scope_dom_node = nullptr;
    if (constraint_scope) {
        line_scope_dom_node = constraint_scope.ptr();
    } else if (topmost_hit_item.has_value()) {
        if (!topmost_hit_facts->can_produce_caret_position() || !item_is_direct_caret_target(topmost_hit_item->index))
            line_scope_dom_node = event_dispatch_dom_node_for_item(topmost_hit_item->index);
    }

    // A constrained search must find a line even when the point is outside the scope's clipped area (e.g. dragging
    // a selection outside a textarea), so it transforms points without rejecting them against clips.
    auto clip_behavior = constraint_scope ? AccumulatedVisualContextTree::ClipBehavior::Ignore : AccumulatedVisualContextTree::ClipBehavior::Respect;

    auto closest_line = find_closest_line(point, document, device_pixels_per_css_pixel, mode, line_scope_dom_node, clip_behavior);
    if (line_scope_dom_node && !constraint_scope) {
        // The scoped search is only a guard against unrelated nearby content. If there is a plainly closer line
        // outside the scope, use it instead.
        auto unscoped_closest_line = find_closest_line(point, document, device_pixels_per_css_pixel, mode, nullptr, clip_behavior);
        if (!closest_line.index.has_value()
            || (unscoped_closest_line.index.has_value() && unscoped_closest_line.block_distance < closest_line.block_distance)) {
            closest_line = unscoped_closest_line;
        }
    }

    if (!closest_line.index.has_value()) {
        if (!constraint_scope && topmost_hit_item.has_value()) {
            auto caret_position = caret_position_for_hit_container(*topmost_hit_facts);
            if (caret_position.has_value() && caret_position->debug_rect.has_value())
                caret_position->debug_rect = viewport_rect_for_context(topmost_hit_facts->context().spatial, *caret_position->debug_rect, document, device_pixels_per_css_pixel);
            return caret_position;
        }
        return {};
    }
    auto caret_position = caret_position_for_line(*closest_line.index, closest_line.local_point, mode);
    if (!caret_position.has_value())
        return {};
    if (caret_position->debug_rect.has_value())
        caret_position->debug_rect = viewport_rect_for_context(caret_line(*closest_line.index).context.spatial, *caret_position->debug_rect, document, device_pixels_per_css_pixel);

    if (!constraint_scope && topmost_hit_item.has_value()) {
        if (auto const* topmost_hit_dom_node = event_dispatch_dom_node_for_item(topmost_hit_item->index); topmost_hit_dom_node && !topmost_hit_dom_node->is_inclusive_ancestor_of(*caret_position->boundary.node)) {
            if (topmost_hit_facts->can_produce_caret_position() && item_is_direct_caret_target(topmost_hit_item->index)) {
                auto caret_position_for_topmost_hit_item = caret_position_for_item(*topmost_hit_facts, topmost_hit_item->local_point);
                if (caret_position_for_topmost_hit_item.has_value() && caret_position_for_topmost_hit_item->debug_rect.has_value())
                    caret_position_for_topmost_hit_item->debug_rect = viewport_rect_for_context(topmost_hit_facts->context().spatial, *caret_position_for_topmost_hit_item->debug_rect, document, device_pixels_per_css_pixel);
                return caret_position_for_topmost_hit_item;
            }
            if (item_is_inline_adjacent_to_line(topmost_hit_item->index, *closest_line.index))
                return caret_position;
            return {};
        }
    }

    return caret_position;
}

Optional<HitTestResult> HitTestDisplayList::hit_test(CSSPixelPoint point, DOM::Document const& document, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics) const
{
    if (m_visual_context_tree_version != document.visual_context_tree().version() || !is_current())
        return {};

    auto topmost_item = find_topmost_item(point, document, device_pixels_per_css_pixel, chrome_metrics);
    if (!topmost_item.has_value())
        return {};
    return hit_test_result_for_item(item(topmost_item->index), topmost_item->local_point);
}

TraversalDecision HitTestDisplayList::hit_test_all(CSSPixelPoint point, DOM::Document const& document, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    if (m_visual_context_tree_version != document.visual_context_tree().version() || !is_current())
        return TraversalDecision::Continue;

    for (auto item_index : hit_item_indices_topmost_first(point, document, device_pixels_per_css_pixel, chrome_metrics)) {
        auto item_facts = item(item_index);
        auto local_point = local_point_for_visual_context(item_facts.context(), point, document, device_pixels_per_css_pixel);
        if (!local_point.has_value())
            continue;
        if (callback(hit_test_result_for_item(item_facts, *local_point)) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    return TraversalDecision::Continue;
}

}
