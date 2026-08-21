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
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/ResizeHandle.h>
#include <LibWeb/Painting/Scrollbar.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Painting {

static bool writing_mode_is_horizontal(CSS::WritingMode writing_mode)
{
    return writing_mode == CSS::WritingMode::HorizontalTb;
}

static CSSPixels block_axis_start(CSSPixelRect rect, CSS::WritingMode writing_mode)
{
    return writing_mode_is_horizontal(writing_mode) ? rect.top() : rect.left();
}

static CSSPixels block_axis_end(CSSPixelRect rect, CSS::WritingMode writing_mode)
{
    return writing_mode_is_horizontal(writing_mode) ? rect.bottom() : rect.right();
}

static CSSPixels block_axis_coordinate(CSSPixelPoint point, CSS::WritingMode writing_mode)
{
    return writing_mode_is_horizontal(writing_mode) ? point.y() : point.x();
}

static CSSPixels inline_axis_start(CSSPixelRect rect, CSS::WritingMode writing_mode)
{
    return writing_mode_is_horizontal(writing_mode) ? rect.left() : rect.top();
}

static CSSPixels inline_axis_end(CSSPixelRect rect, CSS::WritingMode writing_mode)
{
    return writing_mode_is_horizontal(writing_mode) ? rect.right() : rect.bottom();
}

static CSSPixels inline_axis_coordinate(CSSPixelPoint point, CSS::WritingMode writing_mode)
{
    return writing_mode_is_horizontal(writing_mode) ? point.x() : point.y();
}

static bool local_point_is_before_box(Layout::NodeWithStyle const& layout_node, CSSPixelRect rect, CSSPixelPoint local_point)
{
    auto const& style_source = layout_node;
    auto writing_mode = style_source.writing_mode();

    auto block_coordinate = block_axis_coordinate(local_point, writing_mode);
    if (block_coordinate < block_axis_start(rect, writing_mode))
        return !style_source.block_axis_is_reverse();
    if (block_coordinate >= block_axis_end(rect, writing_mode))
        return style_source.block_axis_is_reverse();

    auto inline_start = inline_axis_start(rect, writing_mode);
    auto inline_end = inline_axis_end(rect, writing_mode);
    auto inline_middle = inline_start + (inline_end - inline_start).scaled(0.5);
    auto inline_coordinate = inline_axis_coordinate(local_point, writing_mode);
    return style_source.inline_axis_is_reverse()
        ? inline_coordinate > inline_middle
        : inline_coordinate <= inline_middle;
}

NonnullRefPtr<HitTestDisplayList> HitTestDisplayList::create_from_rust_recording(u64 visual_context_tree_version, Layout::NodeArena& arena)
{
    auto* arena_handle = arena.handle();
    auto list = adopt_ref(*new HitTestDisplayList(visual_context_tree_version, arena, Layout::RustFFI::layout_arena_hit_test_list_generation(arena_handle)));
    auto item_count = Layout::RustFFI::layout_arena_hit_test_item_count(arena_handle);
    list->m_items.ensure_capacity(item_count);
    Vector<Layout::RustFFI::FfiHitTestItemExport> exported_items;
    exported_items.resize(item_count);
    Layout::RustFFI::layout_arena_export_hit_test_items(arena_handle, exported_items.data(), exported_items.size());
    for (auto const& exported : exported_items) {
        VERIFY(exported.paintable.index != Layout::RustFFI::INVALID_PAINTABLE_SLOT_INDEX);
        auto paintable = paintable_for_slot(arena_handle, exported.paintable);
        VERIFY(paintable);
        switch (static_cast<ChromeWidgetKind>(exported.chrome_widget_kind)) {
        case ChromeWidgetKind::None:
            break;
        case ChromeWidgetKind::ResizeHandle:
            (void)paintable->ensure_resize_handle();
            break;
        case ChromeWidgetKind::HorizontalScrollbar:
            (void)paintable->ensure_scrollbar(Paintable::ScrollDirection::Horizontal);
            break;
        case ChromeWidgetKind::VerticalScrollbar:
            (void)paintable->ensure_scrollbar(Paintable::ScrollDirection::Vertical);
            break;
        }
        list->m_items.unchecked_append(Item {
            .kind = static_cast<ItemKind>(exported.kind),
            .box = exported.paintable,
            .chrome_widget_kind = static_cast<ChromeWidgetKind>(exported.chrome_widget_kind),
            .text_fragment_index = exported.has_text_fragment_index ? Optional<u32> { exported.text_fragment_index } : Optional<u32> {},
            .caret_node = exported.caret_node_shell ? static_cast<Layout::Node const*>(exported.caret_node_shell)->dom_node() : nullptr,
            .caret_offset = exported.caret_offset,
            .rect = from_ffi_css_pixel_rect(exported.rect),
            .caret_rect = from_ffi_css_pixel_rect(exported.caret_rect),
            .visual_context_index = VisualContextIndex { exported.visual_context_index },
        });
    }
    return list;
}

HitTestDisplayList::HitTestDisplayList(u64 visual_context_tree_version, Layout::NodeArena& arena, u64 rust_generation)
    : m_visual_context_tree_version(visual_context_tree_version)
    , m_arena(arena)
    , m_rust_generation(rust_generation)
{
}

void HitTestDisplayList::visit_edges(GC::Cell::Visitor& visitor)
{
    for (auto const& item : m_items)
        visitor.visit(item.caret_node);
}

bool HitTestDisplayList::is_current() const
{
    return m_rust_generation != 0 && Layout::RustFFI::layout_arena_hit_test_list_generation(m_arena->handle()) == m_rust_generation;
}

void HitTestDisplayList::ensure_caret_lines() const
{
    if (m_caret_lines_materialized)
        return;
    m_caret_lines_materialized = true;
    if (!is_current())
        return;
    auto* arena = m_arena->handle();
    auto caret_item_count = Layout::RustFFI::layout_arena_hit_test_caret_item_count(arena);
    m_caret_item_indices.ensure_capacity(caret_item_count);
    for (size_t index = 0; index < caret_item_count; ++index)
        m_caret_item_indices.unchecked_append(Layout::RustFFI::layout_arena_hit_test_caret_item_index(arena, index));
    auto line_count = Layout::RustFFI::layout_arena_hit_test_caret_line_count(arena);
    m_caret_lines.ensure_capacity(line_count);
    for (size_t index = 0; index < line_count; ++index) {
        auto exported = Layout::RustFFI::layout_arena_hit_test_caret_line(arena, index);
        m_caret_lines.unchecked_append(CaretLine {
            .rect = from_ffi_css_pixel_rect(exported.rect),
            .visual_context_index = VisualContextIndex { exported.visual_context_index },
            .first_caret_item_index = exported.first_caret_item_index,
            .last_caret_item_index = exported.last_caret_item_index,
        });
    }
}

static Optional<Gfx::FloatPoint> local_float_point_for_visual_context(VisualContextIndex visual_context_index, CSSPixelPoint point, ViewportPaintable const& viewport_paintable, double device_pixels_per_css_pixel, AccumulatedVisualContextTree::ClipBehavior clip_behavior)
{
    auto pixel_ratio = static_cast<float>(device_pixels_per_css_pixel);
    auto const& visual_context_tree = viewport_paintable.visual_context_tree();
    auto result = visual_context_tree.transform_point_for_hit_test(visual_context_index, point.to_type<float>() * pixel_ratio, viewport_paintable.scroll_state_snapshot(), clip_behavior);
    if (!result.has_value())
        return {};
    return *result / pixel_ratio;
}

Optional<CSSPixelPoint> HitTestDisplayList::local_point_for_visual_context(VisualContextIndex visual_context_index, CSSPixelPoint point, ViewportPaintable const& viewport_paintable, double device_pixels_per_css_pixel) const
{
    return local_float_point_for_visual_context(visual_context_index, point, viewport_paintable, device_pixels_per_css_pixel, AccumulatedVisualContextTree::ClipBehavior::Respect)
        .map([](auto float_point) { return float_point.template to_type<CSSPixels>(); });
}

CSSPixelRect HitTestDisplayList::viewport_rect_for_context(VisualContextIndex visual_context_index, CSSPixelRect const& rect, ViewportPaintable const& viewport_paintable, double device_pixels_per_css_pixel) const
{
    auto pixel_ratio = static_cast<float>(device_pixels_per_css_pixel);
    auto const& visual_context_tree = viewport_paintable.visual_context_tree();
    auto result = visual_context_tree.transform_rect_to_viewport(visual_context_index, rect.to_type<float>() * pixel_ratio, viewport_paintable.scroll_state_snapshot());
    return result.scaled(1.0f / pixel_ratio).to_type<CSSPixels>();
}

SortingContexts const& HitTestDisplayList::ensure_sorting_contexts(ViewportPaintable const& viewport_paintable) const
{
    // The version check at every entry point guarantees the tree still matches this list.
    if (!m_sorting_contexts.has_value())
        m_sorting_contexts = viewport_paintable.visual_context_tree().resolve_sorting_contexts();
    return *m_sorting_contexts;
}

struct HitTestDisplayList::QueryContext {
    HitTestDisplayList const& list;
    ViewportPaintable const* viewport_paintable { nullptr };
    double device_pixels_per_css_pixel { 1 };
    ChromeMetrics const* chrome_metrics { nullptr };
    GC::Ptr<DOM::Node const> scope { nullptr };
    HashMap<size_t, Optional<i64>> depth_key_by_plane {};

    Layout::RustFFI::FfiHitTestQueryCallbacks callbacks()
    {
        return {
            .context = this,
            .local_point_for_visual_context = [](void* context_pointer, size_t index, i32 x_raw, i32 y_raw, bool respect_clip, float* out) -> bool {
                auto& context = *static_cast<QueryContext*>(context_pointer);
                VERIFY(context.viewport_paintable);
                auto clip_behavior = respect_clip ? AccumulatedVisualContextTree::ClipBehavior::Respect : AccumulatedVisualContextTree::ClipBehavior::Ignore;
                auto local_point = local_float_point_for_visual_context(VisualContextIndex { index }, { CSSPixels::from_raw(x_raw), CSSPixels::from_raw(y_raw) }, *context.viewport_paintable, context.device_pixels_per_css_pixel, clip_behavior);
                if (!local_point.has_value())
                    return false;
                out[0] = local_point->x();
                out[1] = local_point->y();
                return true;
            },
            .chrome_widget_contains = [](void* context_pointer, void* paintable_shell, u8 kind, i32 x_raw, i32 y_raw) -> bool {
                auto& context = *static_cast<QueryContext*>(context_pointer);
                VERIFY(context.chrome_metrics);
                auto const& paintable = *static_cast<Paintable const*>(paintable_shell);
                CSSPixelPoint local_point { CSSPixels::from_raw(x_raw), CSSPixels::from_raw(y_raw) };
                auto contains = [&](auto widget) { return widget && widget->contains(local_point, *context.chrome_metrics); };
                switch (static_cast<ChromeWidgetKind>(kind)) {
                case ChromeWidgetKind::None:
                    return false;
                case ChromeWidgetKind::ResizeHandle:
                    return contains(paintable.resize_handle());
                case ChromeWidgetKind::HorizontalScrollbar:
                    return contains(paintable.scrollbar(Paintable::ScrollDirection::Horizontal));
                case ChromeWidgetKind::VerticalScrollbar:
                    return contains(paintable.scrollbar(Paintable::ScrollDirection::Vertical));
                }
                VERIFY_NOT_REACHED();
            },
            .line_in_scope = [](void* context_pointer, size_t line_index) -> bool {
                auto& context = *static_cast<QueryContext*>(context_pointer);
                VERIFY(context.scope);
                return context.list.line_contains_descendant_of(context.list.m_caret_lines[line_index], *context.scope);
            },
            .sorting_context_group = [](void* context_pointer, size_t index, size_t* out) -> bool {
                auto& context = *static_cast<QueryContext*>(context_pointer);
                VERIFY(context.viewport_paintable);
                auto const& sorting_contexts = context.list.ensure_sorting_contexts(*context.viewport_paintable);
                if (sorting_contexts.is_empty() || sorting_contexts.leaf_by_node[index] == NO_SORTING_CONTEXT)
                    return false;
                *out = sorting_contexts.outermost_context_of(sorting_contexts.context_by_node[index]).value();
                return true;
            },
            .plane_depth_key = [](void* context_pointer, size_t index, i32 x_raw, i32 y_raw, i64* out) -> bool {
                auto& context = *static_cast<QueryContext*>(context_pointer);
                VERIFY(context.viewport_paintable);
                auto const& sorting_contexts = context.list.ensure_sorting_contexts(*context.viewport_paintable);
                if (sorting_contexts.is_empty())
                    return false;
                auto leaf = sorting_contexts.leaf_by_node[index];
                if (leaf == NO_SORTING_CONTEXT)
                    return false;
                // The plane's depth is the same for every query against one point, so it is resolved once.
                auto depth_key = context.depth_key_by_plane.ensure(leaf.value(), [&]() -> Optional<i64> {
                    CSSPixelPoint point { CSSPixels::from_raw(x_raw), CSSPixels::from_raw(y_raw) };
                    auto device_point = point.to_type<float>() * static_cast<float>(context.device_pixels_per_css_pixel);
                    auto depth = context.viewport_paintable->visual_context_tree().plane_depth_at_point_for_hit_test(leaf, device_point, context.viewport_paintable->scroll_state_snapshot());
                    if (!depth.has_value())
                        return {};
                    static constexpr float depth_limit = 16777216.0f;
                    return llround(clamp(*depth, -depth_limit, depth_limit) * 8.0f);
                });
                if (!depth_key.has_value())
                    return false;
                *out = *depth_key;
                return true;
            },
        };
    }
};

Optional<HitTestDisplayList::TopmostItem> HitTestDisplayList::topmost_item_from(Layout::RustFFI::FfiTopmostItem const& item)
{
    if (!item.has_item)
        return {};
    return TopmostItem { item.index, { CSSPixels::from_raw(item.local_x), CSSPixels::from_raw(item.local_y) } };
}

Optional<HitTestDisplayList::TopmostItem> HitTestDisplayList::find_topmost_item(CSSPixelPoint point, ViewportPaintable const& viewport_paintable, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics) const
{
    QueryContext context { *this, &viewport_paintable, device_pixels_per_css_pixel, &chrome_metrics, nullptr };
    return topmost_item_from(Layout::RustFFI::layout_arena_hit_test_find_topmost_item(m_arena->handle(), context.callbacks(), point.x().raw_value(), point.y().raw_value()));
}

void HitTestDisplayList::find_topmost_items_for_caret(CSSPixelPoint point, ViewportPaintable const& viewport_paintable, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics, Optional<TopmostItem>& caret_item, Optional<TopmostItem>& hit_item) const
{
    QueryContext context { *this, &viewport_paintable, device_pixels_per_css_pixel, &chrome_metrics, nullptr };
    auto items = Layout::RustFFI::layout_arena_hit_test_find_topmost_items_for_caret(m_arena->handle(), context.callbacks(), point.x().raw_value(), point.y().raw_value());
    caret_item = topmost_item_from(items.caret_item);
    hit_item = topmost_item_from(items.hit_item);
}

Vector<size_t> HitTestDisplayList::hit_item_indices_topmost_first(CSSPixelPoint point, ViewportPaintable const& viewport_paintable, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics) const
{
    QueryContext context { *this, &viewport_paintable, device_pixels_per_css_pixel, &chrome_metrics, nullptr };
    Vector<size_t> indices;
    Layout::RustFFI::layout_arena_hit_test_all(m_arena->handle(), context.callbacks(), point.x().raw_value(), point.y().raw_value(), &indices, [](void* sink, size_t index) {
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
    auto result = Layout::RustFFI::layout_arena_hit_test_caret_item_for_line(m_arena->handle(), line_index, local_point.x().raw_value(), local_point.y().raw_value(), to_underlying(mode));
    if (!result.has_item)
        return {};
    return CaretItemForLine { result.item_index, static_cast<CaretPositionType>(result.position_type) };
}

bool HitTestDisplayList::item_is_inline_adjacent_to_line(size_t item_index, size_t line_index) const
{
    return Layout::RustFFI::layout_arena_hit_test_item_is_inline_adjacent_to_line(m_arena->handle(), item_index, line_index);
}

HitTestDisplayList::ClosestLine HitTestDisplayList::find_closest_line(CSSPixelPoint point, ViewportPaintable const& viewport_paintable, double device_pixels_per_css_pixel, CaretPositionMode mode, DOM::Node const* scope_dom_node, AccumulatedVisualContextTree::ClipBehavior clip_behavior) const
{
    QueryContext context { *this, &viewport_paintable, device_pixels_per_css_pixel, nullptr, scope_dom_node };
    auto result = Layout::RustFFI::layout_arena_hit_test_find_closest_line(m_arena->handle(), context.callbacks(), point.x().raw_value(), point.y().raw_value(), to_underlying(mode), scope_dom_node != nullptr, clip_behavior == AccumulatedVisualContextTree::ClipBehavior::Respect);
    ClosestLine closest_line;
    if (result.has_index)
        closest_line.index = result.index;
    closest_line.local_point = { CSSPixels::from_raw(result.local_x), CSSPixels::from_raw(result.local_y) };
    closest_line.block_distance = CSSPixels::from_raw(result.block_distance);
    return closest_line;
}

static Layout::RustFFI::FfiFragmentTextFacts fragment_text_facts(void* arena_handle, Layout::RustFFI::PaintableSlotId box, Optional<u32> const& text_fragment_index)
{
    if (!text_fragment_index.has_value())
        return {};
    return Layout::RustFFI::layout_arena_paintable_fragment_text_facts(arena_handle, box, *text_fragment_index);
}

static Layout::Node const* fragment_layout_node(Layout::RustFFI::FfiFragmentTextFacts const& facts)
{
    return static_cast<Layout::Node const*>(facts.layout_node);
}

Layout::Node const* HitTestDisplayList::layout_node_for_item(Item const& item) const
{
    return static_cast<Layout::Node const*>(Layout::RustFFI::layout_arena_paintable_layout_node_shell(m_arena->handle(), item.box));
}

RefPtr<Paintable> HitTestDisplayList::paintable_for_item(Item const& item) const
{
    return paintable_for_slot(m_arena->handle(), item.box);
}

RefPtr<ChromeWidget> HitTestDisplayList::chrome_widget_for_item(Item const& item) const
{
    auto paintable = paintable_for_item(item);
    if (!paintable)
        return nullptr;
    switch (item.chrome_widget_kind) {
    case ChromeWidgetKind::None:
        return nullptr;
    case ChromeWidgetKind::ResizeHandle:
        return paintable->resize_handle();
    case ChromeWidgetKind::HorizontalScrollbar:
        return paintable->scrollbar(Paintable::ScrollDirection::Horizontal);
    case ChromeWidgetKind::VerticalScrollbar:
        return paintable->scrollbar(Paintable::ScrollDirection::Vertical);
    }
    VERIFY_NOT_REACHED();
}

bool HitTestDisplayList::item_can_produce_caret_position(Item const& item) const
{
    switch (item.kind) {
    case ItemKind::TextFragment: {
        auto const* layout_node = fragment_layout_node(fragment_text_facts(m_arena->handle(), item.box, item.text_fragment_index));
        return layout_node && layout_node->dom_node();
    }
    case ItemKind::EmptyLine:
        return !!item.caret_node;
    case ItemKind::EmptyEditable: {
        auto const* layout_node = layout_node_for_item(item);
        return layout_node && layout_node->dom_node();
    }
    case ItemKind::Box: {
        auto paintable_box = paintable_for_item(item);
        if (!paintable_box)
            return false;
        if (Painting::effective_z_index(paintable_box->layout_node()).value_or(0) < 0)
            return false;
        return paintable_box->dom_node()
            && paintable_box->dom_node()->parent()
            && (paintable_box->layout_node().is_atomic_inline() || paintable_box->layout_node().is_replaced_box());
    }
    case ItemKind::SvgPath:
    case ItemKind::ChromeWidget:
        return false;
    }
    VERIFY_NOT_REACHED();
}

DOM::Node const* HitTestDisplayList::item_dom_node(Item const& item) const
{
    switch (item.kind) {
    case ItemKind::Box:
    case ItemKind::SvgPath:
    case ItemKind::EmptyEditable:
    case ItemKind::ChromeWidget: {
        auto const* layout_node = layout_node_for_item(item);
        return layout_node ? layout_node->dom_node() : nullptr;
    }
    case ItemKind::TextFragment: {
        auto const* layout_node = fragment_layout_node(fragment_text_facts(m_arena->handle(), item.box, item.text_fragment_index));
        return layout_node ? layout_node->dom_node() : nullptr;
    }
    case ItemKind::EmptyLine:
        return item.caret_node.ptr();
    }
    VERIFY_NOT_REACHED();
}

DOM::Node const* HitTestDisplayList::event_dispatch_dom_node_for_item(Item const& item) const
{
    if (item.kind == ItemKind::TextFragment) {
        auto const* layout_node = fragment_layout_node(fragment_text_facts(m_arena->handle(), item.box, item.text_fragment_index));
        if (!layout_node)
            return nullptr;
        if (auto const* node = layout_node->dom_node())
            return node;
        if (layout_node->is_generated_for_pseudo_element())
            return layout_node->pseudo_element_generator().ptr();
        return nullptr;
    }

    if (item.kind == ItemKind::EmptyLine)
        return item_dom_node(item);

    auto* layout_node_shell = Layout::RustFFI::layout_arena_paintable_event_dispatch_node_shell(m_arena->handle(), item.box);
    return layout_node_shell ? static_cast<Layout::Node const*>(layout_node_shell)->dom_node() : nullptr;
}

bool HitTestDisplayList::item_is_direct_caret_target(Item const& item) const
{
    auto const* dom_node = item_dom_node(item);
    return dom_node && dom_node == event_dispatch_dom_node_for_item(item);
}

// https://html.spec.whatwg.org/multipage/image-maps.html#image-map-processing-model
static GC::Ptr<DOM::Node> image_map_area_for_point(Paintable& paintable, CSSPixelPoint local_point)
{
    auto* image_element = as_if<HTML::HTMLImageElement>(paintable.dom_node().ptr());
    if (!image_element)
        return {};

    auto map_element = image_element->associated_map_element();
    if (!map_element)
        return {};

    // For historical reasons, the coordinates must be interpreted relative to the displayed image after any stretching
    // caused by the CSS 'width' and 'height' properties.
    auto image_rect = Painting::absolute_rect(paintable.layout_node());
    return map_element->area_for_point(local_point - image_rect.location(), image_rect.size());
}

HitTestResult HitTestDisplayList::hit_test_result_for_item(Item const& item, CSSPixelPoint local_point) const
{
    switch (item.kind) {
    case ItemKind::Box: {
        GC::Ptr<DOM::Node> node;
        if (auto paintable = paintable_for_item(item))
            node = image_map_area_for_point(*paintable, local_point);
        if (!node)
            node = const_cast<DOM::Node*>(event_dispatch_dom_node_for_item(item));
        return HitTestResult {
            .node = node,
            .box = item.box,
            .arena = *m_arena,
        };
    }
    case ItemKind::SvgPath:
        return HitTestResult {
            .node = const_cast<DOM::Node*>(event_dispatch_dom_node_for_item(item)),
            .box = item.box,
            .arena = *m_arena,
        };
    case ItemKind::TextFragment: {
        VERIFY(item.text_fragment_index.has_value());
        GC::Ptr<DOM::Node> node = const_cast<DOM::Node*>(event_dispatch_dom_node_for_item(item));
        if (!node) {
            auto* layout_node_shell = Layout::RustFFI::layout_arena_paintable_event_dispatch_node_shell(m_arena->handle(), item.box);
            node = layout_node_shell ? static_cast<Layout::Node*>(layout_node_shell)->dom_node() : nullptr;
        }
        return HitTestResult {
            .node = node,
            .box = item.box,
            .arena = *m_arena,
            .index_in_node = Layout::RustFFI::layout_arena_paintable_fragment_index_in_node_for_point(
                m_arena->handle(), item.box, *item.text_fragment_index,
                local_point.x().raw_value(), local_point.y().raw_value()),
            .is_text_fragment = true,
        };
    }
    case ItemKind::EmptyLine:
        // NB: Not reachable through regular hit testing; see item_contains().
        return HitTestResult {
            .node = const_cast<DOM::Node*>(event_dispatch_dom_node_for_item(item)),
            .box = item.box,
            .arena = *m_arena,
        };
    case ItemKind::EmptyEditable:
        return HitTestResult {
            .node = const_cast<DOM::Node*>(event_dispatch_dom_node_for_item(item)),
            .box = item.box,
            .arena = *m_arena,
            .index_in_node = 0,
        };
    case ItemKind::ChromeWidget:
        return HitTestResult {
            .node = const_cast<DOM::Node*>(event_dispatch_dom_node_for_item(item)),
            .box = item.box,
            .arena = *m_arena,
            .chrome_widget = chrome_widget_for_item(item),
        };
    }
    VERIFY_NOT_REACHED();
}

Optional<CaretPosition> HitTestDisplayList::caret_position_for_item(Item const& item, CSSPixelPoint local_point, CaretPositionType type) const
{
    switch (item.kind) {
    case ItemKind::TextFragment: {
        VERIFY(item.text_fragment_index.has_value());
        auto facts = fragment_text_facts(m_arena->handle(), item.box, item.text_fragment_index);
        auto const* fragment_layout = fragment_layout_node(facts);
        auto const* fragment_dom_node = fragment_layout ? fragment_layout->dom_node() : nullptr;
        if (!fragment_dom_node)
            return {};

        auto index_in_node = [&]() -> size_t {
            switch (type) {
            case CaretPositionType::Before:
                return facts.dom_start_offset_in_node;
            case CaretPositionType::After:
                return facts.dom_end_offset_with_trailing_whitespace;
            case CaretPositionType::Closest:
                return Layout::RustFFI::layout_arena_paintable_fragment_index_in_node_for_point(
                    m_arena->handle(), item.box, *item.text_fragment_index,
                    local_point.x().raw_value(), local_point.y().raw_value());
            }
            VERIFY_NOT_REACHED();
        }();

        // A position at the fragment's whitespace-extended end may coincide with the start of the next fragment;
        // Upstream affinity keeps it rendering on this fragment's line.
        auto affinity = index_in_node >= facts.dom_end_offset_in_node && index_in_node == facts.dom_end_offset_with_trailing_whitespace
            ? TextAffinity::Upstream
            : TextAffinity::Downstream;

        auto debug_rect = Layout::RustFFI::layout_arena_paintable_fragment_caret_range_rect(
            m_arena->handle(), item.box, *item.text_fragment_index, index_in_node);
        return CaretPosition {
            .box = item.box,
            .arena = *m_arena,
            .boundary = { const_cast<DOM::Node&>(*fragment_dom_node), static_cast<WebIDL::UnsignedLong>(index_in_node) },
            .affinity = affinity,
            .debug_rect = from_ffi_css_pixel_rect(debug_rect),
        };
    }
    case ItemKind::EmptyLine: {
        auto const* dom_node = item_dom_node(item);
        if (!dom_node)
            return {};
        // An empty line has a single caret position regardless of where on the line the point is.
        return CaretPosition {
            .box = item.box,
            .arena = *m_arena,
            .boundary = { const_cast<DOM::Node&>(*dom_node), static_cast<WebIDL::UnsignedLong>(item.caret_offset) },
            .debug_rect = item.caret_rect,
        };
    }
    case ItemKind::EmptyEditable: {
        auto const* layout_node = layout_node_for_item(item);
        auto* dom_node = const_cast<DOM::Node*>(layout_node ? layout_node->dom_node() : nullptr);
        if (!dom_node)
            return {};
        return CaretPosition {
            .box = item.box,
            .arena = *m_arena,
            .boundary = { *dom_node, 0 },
            .debug_rect = item.caret_rect,
        };
    }
    case ItemKind::Box: {
        auto const* layout_node = layout_node_for_item(item);
        auto const* layout_node_with_style = layout_node ? as_if<Layout::NodeWithStyle>(*layout_node) : nullptr;
        auto* dom_node = const_cast<DOM::Node*>(layout_node ? layout_node->dom_node() : nullptr);
        if (!layout_node_with_style || !dom_node || !dom_node->parent())
            return {};

        auto before_boundary = DOM::BoundaryPoint { *dom_node->parent(), static_cast<WebIDL::UnsignedLong>(dom_node->index()) };
        auto after_boundary = DOM::BoundaryPoint { *dom_node->parent(), static_cast<WebIDL::UnsignedLong>(dom_node->index() + 1) };
        auto point_is_before_box = [&] {
            switch (type) {
            case CaretPositionType::Before:
                return true;
            case CaretPositionType::After:
                return false;
            case CaretPositionType::Closest:
                return local_point_is_before_box(*layout_node_with_style, item.rect, local_point);
            }
            VERIFY_NOT_REACHED();
        }();
        return CaretPosition {
            .box = item.box,
            .arena = *m_arena,
            .boundary = point_is_before_box ? before_boundary : after_boundary,
            .secondary_boundary = point_is_before_box ? after_boundary : before_boundary,
            .debug_rect = item.caret_rect,
        };
    }
    case ItemKind::SvgPath:
    case ItemKind::ChromeWidget:
        return {};
    }
    VERIFY_NOT_REACHED();
}

Optional<CaretPosition> HitTestDisplayList::caret_position_for_hit_container(Item const& item) const
{
    auto dom_node = item_dom_node(item);
    if (!dom_node)
        return {};

    return CaretPosition {
        .box = item.box,
        .arena = *m_arena,
        .boundary = { const_cast<DOM::Node&>(*dom_node), 0 },
        .debug_rect = item.caret_rect,
    };
}

bool HitTestDisplayList::item_contains_caret_position(Item const& item, DOM::Node const& node, size_t offset, TextAffinity affinity) const
{
    switch (item.kind) {
    case ItemKind::TextFragment: {
        VERIFY(item.text_fragment_index.has_value());
        auto facts = fragment_text_facts(m_arena->handle(), item.box, item.text_fragment_index);
        auto const* fragment_layout = fragment_layout_node(facts);
        auto const* fragment_dom_node = fragment_layout ? fragment_layout->dom_node() : nullptr;
        if (!fragment_dom_node)
            return false;
        if (fragment_dom_node == &node) {
            return Layout::RustFFI::layout_arena_paintable_fragment_caret_match(
                       m_arena->handle(), item.box, *item.text_fragment_index,
                       offset, affinity == TextAffinity::Downstream)
                != Layout::RustFFI::FfiCaretMatch::None;
        }

        auto* node_after_boundary = node.child_at_index(offset);
        while (node_after_boundary && node_after_boundary != fragment_dom_node)
            node_after_boundary = node_after_boundary->first_child();
        if (node_after_boundary == fragment_dom_node && facts.dom_start_offset_in_node == 0)
            return true;
        return offset > 0
            && node.child_at_index(offset - 1) == fragment_dom_node
            && facts.dom_end_offset_with_trailing_whitespace == fragment_dom_node->length();
    }
    case ItemKind::EmptyLine:
        return item_dom_node(item) == &node && item.caret_offset == offset;
    case ItemKind::EmptyEditable: {
        auto const* layout_node = layout_node_for_item(item);
        return layout_node && layout_node->dom_node() == &node && offset == 0;
    }
    case ItemKind::Box: {
        auto const* layout_node = layout_node_for_item(item);
        auto dom_node = layout_node ? layout_node->dom_node() : nullptr;
        return dom_node && dom_node->parent() == &node && (offset == dom_node->index() || offset == dom_node->index() + 1);
    }
    case ItemKind::SvgPath:
    case ItemKind::ChromeWidget:
        return false;
    }
    VERIFY_NOT_REACHED();
}

Optional<CaretPosition> HitTestDisplayList::caret_position_at_line_edge(DOM::Node const& node, size_t offset, TextAffinity affinity, CaretLineEdge edge) const
{
    if (!is_current())
        return {};
    ensure_caret_lines();

    auto line_index = caret_line_index_for_position(node, offset, affinity);
    if (!line_index.has_value())
        return {};
    auto type = edge == CaretLineEdge::Start ? CaretPositionType::Before : CaretPositionType::After;
    return caret_position_for_item(m_items[item_index_at_line_edge(*line_index, type)], {}, type);
}

Optional<size_t> HitTestDisplayList::caret_line_index_for_position(DOM::Node const& node, size_t offset, TextAffinity affinity) const
{
    // At a soft wrap, prefer the fragment whose line directly owns the position. Only use the preceding fragment's
    // fallback match when no direct match exists.
    for (bool allow_soft_wrap_fallback : { false, true }) {
        for (size_t line_index = 0; line_index < m_caret_lines.size(); ++line_index) {
            auto const& line = m_caret_lines[line_index];
            for (auto caret_item_index = line.first_caret_item_index; caret_item_index <= line.last_caret_item_index; ++caret_item_index) {
                auto const& item = caret_item(caret_item_index);
                if (!item_contains_caret_position(item, node, offset, affinity))
                    continue;
                if (!allow_soft_wrap_fallback && item.kind == ItemKind::TextFragment && item.text_fragment_index.has_value()) {
                    auto const* fragment_layout = fragment_layout_node(fragment_text_facts(m_arena->handle(), item.box, item.text_fragment_index));
                    if (fragment_layout && fragment_layout->dom_node() == &node
                        && Layout::RustFFI::layout_arena_paintable_fragment_caret_match(
                               m_arena->handle(), item.box, *item.text_fragment_index,
                               offset, affinity == TextAffinity::Downstream)
                            == Layout::RustFFI::FfiCaretMatch::SoftWrapFallback)
                        continue;
                }
                return line_index;
            }
        }
    }
    return {};
}

Optional<CaretPosition> HitTestDisplayList::caret_position_on_adjacent_line(DOM::Node const& node, size_t offset, TextAffinity affinity, CaretLineDirection direction, CSSPixels inline_coordinate, DOM::Node const& scope) const
{
    if (!is_current())
        return {};
    ensure_caret_lines();

    auto current_line_index = caret_line_index_for_position(node, offset, affinity);
    if (!current_line_index.has_value())
        return {};

    // INTEROP: Vertical caret movement in Chromium, WebKit, and Gecko follows rendered line geometry rather than DOM
    QueryContext context { *this, nullptr, 1, nullptr, &scope };
    auto adjacent = Layout::RustFFI::layout_arena_hit_test_adjacent_line(m_arena->handle(), context.callbacks(), *current_line_index, direction == CaretLineDirection::Next ? 1 : 0, inline_coordinate.raw_value());
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
    ensure_caret_lines();

    auto line_index = caret_line_index_for_position(node, offset, affinity);
    if (!line_index.has_value())
        return {};
    return CSSPixels::from_raw(Layout::RustFFI::layout_arena_hit_test_line_block_coordinate(m_arena->handle(), *line_index));
}

Optional<CaretPosition> HitTestDisplayList::caret_position_for_line(size_t line_index, CSSPixelPoint local_point, CaretPositionMode mode) const
{
    auto caret_item = caret_item_for_line(line_index, local_point, mode);
    if (!caret_item.has_value())
        return {};
    return caret_position_for_item(m_items[caret_item->item_index], local_point, caret_item->type);
}

bool HitTestDisplayList::line_contains_descendant_of(CaretLine const& line, DOM::Node const& ancestor) const
{
    for (auto caret_item_index = line.first_caret_item_index; caret_item_index <= line.last_caret_item_index; ++caret_item_index) {
        if (auto const* dom_node = item_dom_node(caret_item(caret_item_index)); dom_node && ancestor.is_inclusive_ancestor_of(*dom_node))
            return true;
    }
    return false;
}

Optional<CaretPosition> HitTestDisplayList::caret_position_from_point(CSSPixelPoint point, ViewportPaintable const& viewport_paintable, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics, CaretPositionMode mode, GC::Ptr<DOM::Node const> constraint_scope) const
{
    if (m_visual_context_tree_version != viewport_paintable.visual_context_tree().version() || !is_current())
        return {};
    ensure_caret_lines();

    // First find both the topmost hit-test item and the topmost item that can directly produce a caret.
    // Non-caret items are still needed to keep later line fallback scoped to the hit content.
    // FIXME: Caret placement compares items by record order alone, ignoring the depth-sorted paint order of
    //        planes inside 3D rendering contexts.
    Optional<TopmostItem> topmost_item;
    Optional<TopmostItem> topmost_hit_item;
    find_topmost_items_for_caret(point, viewport_paintable, device_pixels_per_css_pixel, chrome_metrics, topmost_item, topmost_hit_item);

    // A constrained search only accepts direct hits inside the constraint scope.
    if (constraint_scope && topmost_item.has_value()) {
        auto const* item_node = item_dom_node(m_items[topmost_item->index]);
        if (!item_node || !constraint_scope->is_inclusive_ancestor_of(*item_node))
            topmost_item = {};
    }

    // Direct caret hits win unless another non-caret item is visibly on top of them.
    auto topmost_caret_item_matches_hit_item = [&] {
        return topmost_hit_item.has_value()
            && topmost_item->index == topmost_hit_item->index
            && item_is_direct_caret_target(m_items[topmost_item->index]);
    };
    if (topmost_item.has_value() && (constraint_scope || !topmost_hit_item.has_value() || topmost_caret_item_matches_hit_item())) {
        auto const& item = m_items[topmost_item->index];
        if (auto caret_position = caret_position_for_item(item, topmost_item->local_point); caret_position.has_value()) {
            if (caret_position->debug_rect.has_value())
                caret_position->debug_rect = viewport_rect_for_context(item.visual_context_index, *caret_position->debug_rect, viewport_paintable, device_pixels_per_css_pixel);
            return caret_position;
        }
    }

    // If the point is over a non-caret item, only consider caret lines inside that item's event-dispatch node first.
    // This prevents overlays or side content from snapping the caret to unrelated nearby text.
    DOM::Node const* line_scope_dom_node = nullptr;
    if (constraint_scope) {
        line_scope_dom_node = constraint_scope.ptr();
    } else if (topmost_hit_item.has_value()) {
        auto const& topmost_hit_item_value = m_items[topmost_hit_item->index];
        if (!item_can_produce_caret_position(topmost_hit_item_value) || !item_is_direct_caret_target(topmost_hit_item_value))
            line_scope_dom_node = event_dispatch_dom_node_for_item(topmost_hit_item_value);
    }

    // A constrained search must find a line even when the point is outside the scope's clipped area (e.g. dragging
    // a selection outside a textarea), so it transforms points without rejecting them against clips.
    auto clip_behavior = constraint_scope ? AccumulatedVisualContextTree::ClipBehavior::Ignore : AccumulatedVisualContextTree::ClipBehavior::Respect;

    auto closest_line = find_closest_line(point, viewport_paintable, device_pixels_per_css_pixel, mode, line_scope_dom_node, clip_behavior);
    if (line_scope_dom_node && !constraint_scope) {
        // The scoped search is only a guard against unrelated nearby content. If there is a plainly closer line
        // outside the scope, use it instead.
        auto unscoped_closest_line = find_closest_line(point, viewport_paintable, device_pixels_per_css_pixel, mode, nullptr, clip_behavior);
        if (!closest_line.index.has_value()
            || (unscoped_closest_line.index.has_value() && unscoped_closest_line.block_distance < closest_line.block_distance)) {
            closest_line = unscoped_closest_line;
        }
    }

    if (!closest_line.index.has_value()) {
        if (!constraint_scope && topmost_hit_item.has_value()) {
            auto const& item = m_items[topmost_hit_item->index];
            auto caret_position = caret_position_for_hit_container(item);
            if (caret_position.has_value() && caret_position->debug_rect.has_value())
                caret_position->debug_rect = viewport_rect_for_context(item.visual_context_index, *caret_position->debug_rect, viewport_paintable, device_pixels_per_css_pixel);
            return caret_position;
        }
        return {};
    }
    auto caret_position = caret_position_for_line(*closest_line.index, closest_line.local_point, mode);
    if (!caret_position.has_value())
        return {};
    if (caret_position->debug_rect.has_value())
        caret_position->debug_rect = viewport_rect_for_context(m_caret_lines[*closest_line.index].visual_context_index, *caret_position->debug_rect, viewport_paintable, device_pixels_per_css_pixel);

    if (!constraint_scope && topmost_hit_item.has_value()) {
        auto const& topmost_hit_item_value = m_items[topmost_hit_item->index];
        if (auto const* topmost_hit_dom_node = event_dispatch_dom_node_for_item(topmost_hit_item_value); topmost_hit_dom_node && !topmost_hit_dom_node->is_inclusive_ancestor_of(*caret_position->boundary.node)) {
            if (item_can_produce_caret_position(topmost_hit_item_value) && item_is_direct_caret_target(topmost_hit_item_value)) {
                auto caret_position_for_topmost_hit_item = caret_position_for_item(topmost_hit_item_value, topmost_hit_item->local_point);
                if (caret_position_for_topmost_hit_item.has_value() && caret_position_for_topmost_hit_item->debug_rect.has_value())
                    caret_position_for_topmost_hit_item->debug_rect = viewport_rect_for_context(topmost_hit_item_value.visual_context_index, *caret_position_for_topmost_hit_item->debug_rect, viewport_paintable, device_pixels_per_css_pixel);
                return caret_position_for_topmost_hit_item;
            }
            if (item_is_inline_adjacent_to_line(topmost_hit_item->index, *closest_line.index))
                return caret_position;
            return {};
        }
    }

    return caret_position;
}

Optional<HitTestResult> HitTestDisplayList::hit_test(CSSPixelPoint point, ViewportPaintable const& viewport_paintable, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics) const
{
    if (m_visual_context_tree_version != viewport_paintable.visual_context_tree().version() || !is_current())
        return {};

    auto topmost_item = find_topmost_item(point, viewport_paintable, device_pixels_per_css_pixel, chrome_metrics);
    if (!topmost_item.has_value())
        return {};
    return hit_test_result_for_item(m_items[topmost_item->index], topmost_item->local_point);
}

TraversalDecision HitTestDisplayList::hit_test_all(CSSPixelPoint point, ViewportPaintable const& viewport_paintable, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    if (m_visual_context_tree_version != viewport_paintable.visual_context_tree().version() || !is_current())
        return TraversalDecision::Continue;

    for (auto item_index : hit_item_indices_topmost_first(point, viewport_paintable, device_pixels_per_css_pixel, chrome_metrics)) {
        auto const& item = m_items[item_index];
        auto local_point = local_point_for_visual_context(item.visual_context_index, point, viewport_paintable, device_pixels_per_css_pixel);
        if (!local_point.has_value())
            continue;
        if (callback(hit_test_result_for_item(item, *local_point)) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    return TraversalDecision::Continue;
}

}
