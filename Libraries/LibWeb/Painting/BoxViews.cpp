/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextOffsetMapping.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/SVG/SVGFilterElement.h>

namespace Web::Painting {

static bool g_paint_viewport_scrollbars = true;

void set_paint_viewport_scrollbars(bool enabled)
{
    g_paint_viewport_scrollbars = enabled;
}

bool should_paint_viewport_scrollbars()
{
    return g_paint_viewport_scrollbars;
}

bool body_background_is_propagated_to_root(Layout::NodeWithStyle const& layout_node)
{
    if (!layout_node.is_body())
        return false;
    // Reachable at invalidation time, when the root element's layout node may already be detached.
    auto const* html_element = layout_node.document().html_element();
    return html_element && html_element->unsafe_layout_node() && html_element->should_use_body_background_properties();
}

ResolvedCSSFilter resolve_css_filter(CSS::ComputedFilterView computed_filter, Layout::NodeWithStyle const& layout_node)
{
    ResolvedCSSFilter result;
    bool failed = false;
    computed_filter.for_each_operation([&](auto const& operation) {
        if (failed)
            return;
        if (auto const* url = operation.template get_pointer<CSS::Filter::Url>()) {
            if (url->fragment.is_empty()) {
                failed = true;
                return;
            }
            auto maybe_filter = layout_node.document().get_element_by_id(url->fragment);
            if (!maybe_filter) {
                failed = true;
                return;
            }
            if (auto* filter_element = as_if<SVG::SVGFilterElement>(*maybe_filter)) {
                // Filter primitive lengths are specified in the filtered element's user coordinate system, but the
                // resulting filter operates in device pixels. Compute the user-unit-to-device-pixel scale so the
                // filter can convert its lengths accordingly.
                // The replay-time layer maps filter parameters through the accumulated transform,
                // so only the device pixel ratio — which lives in recorded coordinates, not in the
                // transform chain — converts here.
                auto device_pixels_per_css_pixel = layout_node.document().page().client().device_pixels_per_css_pixel();
                auto filter_scale = Gfx::FloatPoint { device_pixels_per_css_pixel, device_pixels_per_css_pixel };
                result.svg_filter = filter_element->gfx_filter(layout_node, filter_scale);
                // The bounds live in the filtered element's user space; an element without
                // geometry of its own falls back to the whole enclosing viewport rect there.
                auto bounds = absolute_border_box_rect(layout_node);
                if (bounds.is_empty()) {
                    auto viewport_rect = Layout::RustFFI::layout_arena_paintable_svg_viewport_user_rect(layout_node.arena_handle(), committed_row_slot(layout_node));
                    if (viewport_rect.has_value())
                        result.svg_filter_bounds = viewport_rect.value();
                }
                if (!bounds.is_empty())
                    result.svg_filter_bounds = bounds;
            } else {
                failed = true;
            }
            return;
        }

        operation.visit(
            [&](CSS::Filter::Blur const& blur) {
                result.operations.empend(ResolvedCSSFilter::Blur {
                    .radius = CSSPixels::nearest_value_for(blur.resolved_radius),
                });
            },
            [&](CSS::Filter::DropShadow const& drop_shadow) {
                result.operations.empend(ResolvedCSSFilter::DropShadow {
                    .offset_x = drop_shadow.offset_x,
                    .offset_y = drop_shadow.offset_y,
                    .radius = drop_shadow.radius,
                    .color = drop_shadow.color,
                });
            },
            [&](CSS::Filter::ColorOperation const& color_operation) {
                result.operations.empend(ResolvedCSSFilter::Color {
                    .operation = color_operation.operation,
                    .amount = color_operation.resolved_amount,
                });
            },
            [&](CSS::Filter::HueRotate const& hue_rotate) {
                result.operations.empend(ResolvedCSSFilter::HueRotate {
                    .angle_degrees = hue_rotate.angle_degrees,
                });
            },
            [&](CSS::Filter::Url const&) {});
    });
    if (failed)
        return {};
    return result;
}

Layout::RustFFI::NodeSlotId committed_row_slot(Layout::Node const& node)
{
    return Layout::Node::slot_id(&node);
}

Layout::RustFFI::NodeSlotId viewport_row_slot(DOM::Document const& document)
{
    return Layout::Node::slot_id(document.unsafe_layout_node());
}

Layout::RustFFI::PaintableData const* committed_row(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_row(node.arena_handle(), committed_row_slot(node));
}

bool has_committed_box(Layout::Node const& node)
{
    return committed_row(node) != nullptr;
}

Layout::Node* layout_node_for_committed_slot(Layout::NodeArena& arena, Layout::RustFFI::NodeSlotId slot)
{
    return static_cast<Layout::Node*>(Layout::RustFFI::layout_arena_paintable_layout_node_shell(arena.handle(), slot));
}

static PixelBox pixel_box_from_ffi(Layout::RustFFI::FfiPixelBox const& box)
{
    return { box.top, box.right, box.bottom, box.left };
}

CSSPixelRect absolute_rect(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_absolute_rect(node.arena_handle(), committed_row_slot(node));
}

CSSPixelRect absolute_padding_box_rect(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_absolute_padding_box_rect(node.arena_handle(), committed_row_slot(node));
}

CSSPixelRect absolute_border_box_rect(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_absolute_border_box_rect(node.arena_handle(), committed_row_slot(node));
}

CSSPixelPoint absolute_position(Layout::Node const& node)
{
    return absolute_rect(node).location();
}

CSSPixelPoint offset(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_offset(node.arena_handle(), committed_row_slot(node));
}

CSSPixelSize content_size(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_content_size(node.arena_handle(), committed_row_slot(node));
}

CSSPixels content_width(Layout::Node const& node)
{
    return content_size(node).width();
}

CSSPixels content_height(Layout::Node const& node)
{
    return content_size(node).height();
}

BoxModelMetrics box_model(Layout::Node const& node)
{
    auto metrics = Layout::RustFFI::layout_arena_paintable_box_model(node.arena_handle(), committed_row_slot(node));
    return {
        .margin = pixel_box_from_ffi(metrics.margin),
        .padding = pixel_box_from_ffi(metrics.padding),
        .border = pixel_box_from_ffi(metrics.border),
        .inset = pixel_box_from_ffi(metrics.inset),
    };
}

CSSPixels border_box_width(Layout::Node const& node)
{
    auto border_box = box_model(node).border_box();
    return content_width(node) + border_box.left + border_box.right;
}

CSSPixels border_box_height(Layout::Node const& node)
{
    auto border_box = box_model(node).border_box();
    return content_height(node) + border_box.top + border_box.bottom;
}

Optional<OverflowData> overflow_data(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row || !row->overflow_measured_this_commit)
        return {};
    return OverflowData { row->overflow_relative_to_padding_box.rect, row->overflow_relative_to_padding_box.has_scrollable_overflow };
}

static void measure_scrollable_overflow_if_missing(Layout::Node const& node, Layout::RustFFI::PaintableData const& row)
{
    if (row.overflow_measured_this_commit || row.overflow_valid_across_recommits)
        return;
    if (auto const* box = as_if<Layout::Box>(node))
        rust_measure_scrollable_overflow(*box);
}

bool has_scrollable_overflow(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row)
        return false;
    measure_scrollable_overflow_if_missing(node, *row);
    return (row->overflow_measured_this_commit || row->overflow_valid_across_recommits) && row->overflow_relative_to_padding_box.has_scrollable_overflow;
}

Optional<CSSPixelRect> scrollable_overflow_rect(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row)
        return {};
    measure_scrollable_overflow_if_missing(node, *row);
    if (!row->overflow_measured_this_commit && !row->overflow_valid_across_recommits)
        return {};
    auto rect = row->overflow_relative_to_padding_box.rect;
    rect.translate_by(absolute_padding_box_rect(node).location());
    return rect;
}

bool is_visible(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return false;
    auto const& styled_node = as<Layout::NodeWithStyle>(node);
    return styled_node.visibility() == CSS::Visibility::Visible && styled_node.opacity() != 0;
}

bool visible_for_hit_testing(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return false;
    if (auto dom_node = node.dom_node(); dom_node && dom_node->is_inert())
        return false;
    return as<Layout::NodeWithStyle>(node).pointer_events() != CSS::PointerEvents::None;
}

bool has_stacking_context(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row && row->stacking_context != Layout::RustFFI::NO_STACKING_CONTEXT;
}

CSS::Display display(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return {};
    return as<Layout::NodeWithStyle>(node).display();
}

bool is_positioned(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_is_positioned(node.arena_handle(), committed_row_slot(node));
}

bool is_fixed_position(Layout::Node const& node)
{
    return has_committed_box(node) && as<Layout::NodeWithStyle>(node).is_fixed_position();
}

bool has_css_transform(Layout::Node const& node)
{
    return has_committed_box(node) && as<Layout::NodeWithStyle>(node).has_css_transform();
}

bool uses_collapsing_borders_model(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_uses_collapsing_borders_model(node.arena_handle(), committed_row_slot(node));
}

SelectionState selection_state(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row)
        return {};
    return static_cast<SelectionState>(row->selection_state);
}

CSS::StyleRecordID style_record_identity(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return {};
    return as<Layout::NodeWithStyle>(node).style_record_identity();
}

static StringView class_name_for_kind(Layout::RustFFI::PaintableKind kind)
{
    switch (kind) {
    case Layout::RustFFI::PaintableKind::None:
    case Layout::RustFFI::PaintableKind::Paintable:
        return "Paintable"sv;
    case Layout::RustFFI::PaintableKind::PaintableWithLines:
        return "PaintableWithLines"sv;
    case Layout::RustFFI::PaintableKind::InlinePaintable:
        return "InlinePaintable"sv;
    case Layout::RustFFI::PaintableKind::ViewportPaintable:
        return "ViewportPaintable"sv;
    case Layout::RustFFI::PaintableKind::ImagePaintable:
        return "ImagePaintable"sv;
    case Layout::RustFFI::PaintableKind::CanvasPaintable:
        return "CanvasPaintable"sv;
    case Layout::RustFFI::PaintableKind::VideoPaintable:
        return "VideoPaintable"sv;
    case Layout::RustFFI::PaintableKind::CheckBoxPaintable:
        return "CheckBoxPaintable"sv;
    case Layout::RustFFI::PaintableKind::RadioButtonPaintable:
        return "RadioButtonPaintable"sv;
    case Layout::RustFFI::PaintableKind::FieldSetPaintable:
        return "FieldSetPaintable"sv;
    case Layout::RustFFI::PaintableKind::NavigableContainerViewportPaintable:
        return "NavigableContainerViewportPaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGSVGPaintable:
        return "SVGSVGPaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGPathPaintable:
        return "SVGPathPaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGGraphicsPaintable:
        return "SVGGraphicsPaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGImagePaintable:
        return "SVGImagePaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGMaskPaintable:
        return "SVGMaskPaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGClipPaintable:
        return "SVGClipPaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGPatternPaintable:
        return "SVGPatternPaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGForeignObjectPaintable:
        return "SVGForeignObjectPaintable"sv;
    }
    VERIFY_NOT_REACHED();
}

StringView class_name(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row ? class_name_for_kind(row->kind) : StringView {};
}

String debug_description(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row)
        return {};
    return MUST(String::formatted("{}({})", class_name_for_kind(row->kind), node.debug_description()));
}

bool is_navigable_container_viewport_paintable(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row && row->kind == Layout::RustFFI::PaintableKind::NavigableContainerViewportPaintable;
}

bool is_viewport_paintable(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row && row->kind == Layout::RustFFI::PaintableKind::ViewportPaintable;
}

bool is_paintable_with_lines(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row)
        return false;
    auto kind = row->kind;
    return kind == Layout::RustFFI::PaintableKind::PaintableWithLines
        || kind == Layout::RustFFI::PaintableKind::ViewportPaintable
        || kind == Layout::RustFFI::PaintableKind::SVGForeignObjectPaintable;
}

bool is_inline_paintable(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row && row->kind == Layout::RustFFI::PaintableKind::InlinePaintable;
}

bool is_svg_paintable(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row)
        return false;
    switch (row->kind) {
    case Layout::RustFFI::PaintableKind::SVGGraphicsPaintable:
    case Layout::RustFFI::PaintableKind::SVGPathPaintable:
    case Layout::RustFFI::PaintableKind::SVGImagePaintable:
    case Layout::RustFFI::PaintableKind::SVGMaskPaintable:
    case Layout::RustFFI::PaintableKind::SVGClipPaintable:
    case Layout::RustFFI::PaintableKind::SVGPatternPaintable:
        return true;
    default:
        return false;
    }
}

bool is_svg_svg_paintable(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row && row->kind == Layout::RustFFI::PaintableKind::SVGSVGPaintable;
}

bool is_svg_path_paintable(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row && row->kind == Layout::RustFFI::PaintableKind::SVGPathPaintable;
}

Optional<int> effective_z_index(Layout::Node const& node)
{
    if (!is_positioned(node))
        return {};
    return as<Layout::NodeWithStyle>(node).z_index();
}

bool has_accumulated_visual_context(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row && row->has_accumulated_visual_context;
}

ContextRef accumulated_visual_context(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row ? row->accumulated_visual_context : ContextRef {};
}

ContextRef accumulated_visual_context_for_descendants(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row ? row->accumulated_visual_context_for_descendants : ContextRef {};
}

Optional<ContextRef> fixed_background_visual_context(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row || !row->has_fixed_background_visual_context)
        return {};
    return row->fixed_background_visual_context;
}

SpatialNodeIndex enclosing_scroll_node_index(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row ? row->enclosing_scroll_node_index : VISUAL_VIEWPORT_NODE_INDEX;
}

SpatialNodeIndex own_scroll_node_index(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row ? row->own_scroll_node_index : VISUAL_VIEWPORT_NODE_INDEX;
}

Gfx::Path const* committed_svg_path(Layout::Node const& node)
{
    return static_cast<Gfx::Path const*>(Layout::RustFFI::layout_arena_paintable_computed_svg_path(node.arena_handle(), committed_row_slot(node)));
}

CSSPixelSize svg_viewport_size(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_svg_viewport_size(node.arena_handle(), committed_row_slot(node));
}

Optional<Gfx::AffineTransform> svg_viewport_transform(Layout::Node const& node)
{
    auto result = Layout::RustFFI::layout_arena_paintable_svg_viewport_transform(node.arena_handle(), committed_row_slot(node));
    if (!result.has_value)
        return {};
    auto const& transform = result.transform;
    return Gfx::AffineTransform { transform.a, transform.b, transform.c, transform.d, transform.e, transform.f };
}

Optional<UsedGridTrackList> used_values_for_grid_template_columns(Layout::Node const& node)
{
    Optional<UsedGridTrackList> result;
    Layout::RustFFI::layout_arena_paintable_used_grid_tracks(node.arena_handle(), committed_row_slot(node), &result,
        [](void* context, Layout::RustFFI::FfiUsedGridTrackList const* columns, Layout::RustFFI::FfiUsedGridTrackList const*) {
            *static_cast<Optional<UsedGridTrackList>*>(context) = Layout::build_used_grid_track_list(*columns);
        });
    return result;
}

Optional<UsedGridTrackList> used_values_for_grid_template_rows(Layout::Node const& node)
{
    Optional<UsedGridTrackList> result;
    Layout::RustFFI::layout_arena_paintable_used_grid_tracks(node.arena_handle(), committed_row_slot(node), &result,
        [](void* context, Layout::RustFFI::FfiUsedGridTrackList const*, Layout::RustFFI::FfiUsedGridTrackList const* rows) {
            *static_cast<Optional<UsedGridTrackList>*>(context) = Layout::build_used_grid_track_list(*rows);
        });
    return result;
}

CSSPixelPoint box_type_agnostic_position(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row)
        return {};
    if (row->kind == Layout::RustFFI::PaintableKind::InlinePaintable) {
        auto result = Layout::RustFFI::layout_arena_inline_paintable_first_piece_position(node.arena_handle(), committed_row_slot(node));
        if (result.has_value)
            return { result.x, result.y };
    }
    return absolute_position(node);
}

SelectionStyle selection_style(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return {};
    return selection_style_for_node(node, node.dom_node());
}

bool has_sticky_insets(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    return row && row->has_sticky_insets;
}

StickyInsets sticky_insets(Layout::Node const& node)
{
    auto const* row = committed_row(node);
    if (!row)
        return {};
    VERIFY(row->has_sticky_insets);
    auto const& insets = row->sticky_insets;
    auto side = [](CSSPixels value, bool present) -> Optional<CSSPixels> {
        if (!present)
            return {};
        return value;
    };
    return { side(insets.top, insets.has_top), side(insets.right, insets.has_right), side(insets.bottom, insets.has_bottom), side(insets.left, insets.has_left) };
}

bool should_paint_cursor(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return false;

    auto const& document = node.document();
    if (!document.cursor_blink_state() || !document.navigable()->is_focused())
        return false;

    auto cursor_position = document.cursor_position();
    if (!cursor_position)
        return false;

    if (auto const* text_control = as_if<HTML::FormAssociatedTextControlElement>(document.focused_area().ptr());
        text_control && text_control->text_control_to_html_element().is_mutable()) {
        return true;
    }

    // The editable element may sit anywhere between the cursor and this box (e.g. a
    // contenteditable inline box), so editability is the cursor node's, not this box's.
    auto const* editable_node = cursor_position->node().ptr();
    return editable_node && editable_node->is_editable_or_editing_host();
}

Layout::Node const* nearest_self_painting_inline_box(Layout::Node const& node)
{
    for (auto const* ancestor = node.nearest_fragmented_inline_ancestor(); ancestor; ancestor = ancestor->nearest_fragmented_inline_ancestor()) {
        auto const* row = committed_row(*ancestor);
        if (row && row->kind == Layout::RustFFI::PaintableKind::InlinePaintable
            && (row->stacking_context != Layout::RustFFI::NO_STACKING_CONTEXT || is_positioned(*ancestor)))
            return ancestor;
    }
    return nullptr;
}

bool has_content(Layout::Node const& node)
{
    // Interrupting block-in-inline children produce only placeholder pieces, so any child
    // paintable also counts as content.
    return Layout::RustFFI::layout_arena_inline_paintable_has_content_pieces(node.arena_handle(), committed_row_slot(node))
        || Layout::RustFFI::layout_arena_paintable_has_child_paintables(node.arena_handle(), committed_row_slot(node));
}

// Caret rect for a cursor parked on this paintable's DOM node at the given child offset, e.g. on an empty line
// rendered by a <br> child or in an empty editable element.
CSSPixelRect caret_rect_for_child_offset(Layout::Node const& block, size_t offset)
{
    if (!has_committed_box(block))
        return {};
    auto const& styled_block = as<Layout::NodeWithStyle>(block);

    auto content_box = absolute_padding_box_rect(block);
    auto line_height = styled_block.line_height();
    CSSPixelRect rect { content_box.x(), content_box.y(), 1, line_height };

    auto dom_node = block.dom_node();
    if (!dom_node)
        return rect;

    // A boundary immediately after an atomic inline element paints after that element. Atomic inline elements have
    if (offset > 0) {
        auto* previous_child = dom_node->child_at_index(offset - 1);
        auto const* previous_layout_node = previous_child ? previous_child->unsafe_layout_node() : nullptr;
        if (previous_layout_node && previous_layout_node->is_atomic_inline()) {
            auto result = Layout::RustFFI::layout_arena_paintable_first_fragment_rect_for_node(block.arena_handle(), committed_row_slot(block), Layout::Node::slot_id(previous_layout_node));
            if (result.has_value) {
                auto fragment_rect = result.rect;
                if (styled_block.writing_mode() == CSS::WritingMode::HorizontalTb)
                    rect.set_x(styled_block.inline_axis_is_reverse() ? fragment_rect.left() : fragment_rect.right());
                else
                    rect.set_y(styled_block.inline_axis_is_reverse() ? fragment_rect.top() : fragment_rect.bottom());
                return rect;
            }
        }
    }

    auto* child = dom_node->child_at_index(offset);
    if (!child || !is<HTML::HTMLBRElement>(*child))
        return rect;

    // A caret parked before a <br> sits on the line below the content preceding the <br>. Layout produces no
    // fragments for <br>, so start below the fragments of any preceding content, and add one line height for each
    // empty line rendered by earlier <br>s.
    struct PrecedingContentContext {
        GC::Ref<DOM::Node> child;
        Optional<CSSPixels> preceding_content_bottom;
    } preceding_context { const_cast<DOM::Node&>(*child), {} };
    Layout::RustFFI::layout_arena_for_each_subtree_fragment_rect(
        block.arena_handle(), committed_row_slot(block), &preceding_context,
        [](void* context_pointer, void* fragment_layout_node_shell, CSSPixelRect rect) {
            auto& context = *static_cast<PrecedingContentContext*>(context_pointer);
            auto const* fragment_layout_node = static_cast<Layout::Node const*>(fragment_layout_node_shell);
            auto* fragment_dom_node = fragment_layout_node ? const_cast<DOM::Node*>(fragment_layout_node->dom_node()) : nullptr;
            if (!fragment_dom_node || !(context.child->compare_document_position(fragment_dom_node) & DOM::Node::DOCUMENT_POSITION_PRECEDING))
                return;
            auto bottom = rect.bottom();
            if (!context.preceding_content_bottom.has_value() || bottom > *context.preceding_content_bottom)
                context.preceding_content_bottom = bottom;
        });
    auto& preceding_content_bottom = preceding_context.preceding_content_bottom;

    size_t preceding_empty_lines = 0;
    dom_node->for_each_in_subtree_of_type<HTML::HTMLBRElement>([&](auto& br) {
        if (&br == child)
            return TraversalDecision::Break;
        if (br.represents_empty_line())
            ++preceding_empty_lines;
        return TraversalDecision::Continue;
    });

    rect.set_y(preceding_content_bottom.value_or(content_box.y()) + line_height * preceding_empty_lines);
    return rect;
}

static bool layout_node_is_visible(Layout::NodeWithStyle const& layout_node)
{
    return layout_node.visibility() == CSS::Visibility::Visible && layout_node.opacity() != 0;
}

Optional<CaretPaint> resolve_caret_paint(Layout::Node const& block, Layout::Node const* owner_inline)
{
    if (!should_paint_cursor(block))
        return {};
    auto const& styled_block = as<Layout::NodeWithStyle>(block);

    auto cursor_position = block.document().cursor_position();
    VERIFY(cursor_position);

    auto const* dom_node = block.dom_node();

    Vector<Layout::RustFFI::NodeSlotId, 2> text_slots;
    if (auto const* text = as_if<DOM::Text>(cursor_position->node().ptr()))
        text_slots = Layout::TextOffsetMapping { *text }.slot_ids();

    if (!text_slots.is_empty()) {
        auto result = Layout::RustFFI::layout_arena_text_caret_rect_for_position(
            block.arena_handle(), text_slots.data(), text_slots.size(), cursor_position->offset(),
            cursor_position->affinity() == TextAffinity::Downstream);
        if (result.found && result.owner_paintable.index == committed_row_slot(block).index) {
            auto owner_slot = owner_inline ? committed_row_slot(*owner_inline)
                                           : Layout::RustFFI::NodeSlotId { Layout::RustFFI::INVALID_NODE_SLOT_INDEX };
            if (result.nearest_self_painting_inline.index != owner_slot.index)
                return {};
            auto const* style_source = static_cast<Layout::NodeWithStyle const*>(result.style_source);
            if (!style_source || !layout_node_is_visible(*style_source))
                return {};
            return CaretPaint { result.rect, style_source->caret_color() };
        }
        if (result.found) {
            return {};
        }
    }

    if (owner_inline) {
        // Blank lines and empty editable elements are handled by the block / the box itself.
        return {};
    }
    if (!is_visible(block)) {
        // Blank-line and empty-element carets belong to this block itself.
        return {};
    }

    if (!text_slots.is_empty()) {
        auto empty_line = Layout::RustFFI::layout_arena_paintable_empty_line_caret_rect(
            block.arena_handle(), committed_row_slot(block), text_slots.data(), text_slots.size(), cursor_position->offset());
        if (empty_line.has_value) {
            auto const* style_source = static_cast<Layout::NodeWithStyle const*>(empty_line.style_source);
            if (!style_source)
                return {};
            auto empty_line_rect = empty_line.rect;
            CSSPixelRect cursor_rect { empty_line_rect.x(), empty_line_rect.y(), 1, empty_line_rect.height() };
            return CaretPaint { cursor_rect, style_source->caret_color() };
        }
    }

    if (cursor_position->node() != GC::Ptr { dom_node })
        return {};

    return CaretPaint { caret_rect_for_child_offset(block, cursor_position->offset()), styled_block.caret_color() };
}

Optional<CaretPaint> resolve_empty_editable_caret_paint(Layout::Node const& node)
{
    if (!should_paint_cursor(node) || has_content(node))
        return {};
    auto const& styled_node = as<Layout::NodeWithStyle>(node);

    auto cursor_position = node.document().cursor_position();
    VERIFY(cursor_position);

    auto const* dom_node = node.dom_node();
    if (!dom_node || cursor_position->node() != GC::Ptr { dom_node })
        return {};

    auto position = box_type_agnostic_position(node);
    return CaretPaint {
        .rect = { position.x(), position.y(), 1, styled_node.line_height() },
        .color = styled_node.caret_color(),
    };
}

static Optional<CSS::BorderData> border_data_for_outline(Layout::Node const& layout_node, Color outline_color, CSS::OutlineStyle outline_style, CSSPixels outline_width)
{
    CSS::LineStyle line_style;
    if (outline_style == CSS::OutlineStyle::Auto) {
        line_style = CSS::LineStyle::Solid;
        outline_color = CSS::KeywordStyleValue::create(CSS::Keyword::Accentcolor)->to_color(CSS::ColorResolutionContext::for_layout_node_with_style(*static_cast<Layout::NodeWithStyle const*>(&layout_node))).value();
        outline_width = 2;
    } else {
        line_style = CSS::keyword_to_line_style(CSS::to_keyword(outline_style)).value_or(CSS::LineStyle::None);
    }

    if (outline_color.alpha() == 0 || line_style == CSS::LineStyle::None || outline_width == 0)
        return {};

    return CSS::BorderData {
        .color = outline_color,
        .line_style = line_style,
        .width = outline_width,
    };
}

Optional<CSS::BorderData> outline_data(Layout::Node const& node, CSS::ComputedValues const& computed_values)
{
    if (!has_committed_box(node))
        return {};

    // The `auto` outline is the UA focus ring; like native controls, it is only shown while the window has focus.
    auto navigable = node.document().navigable();
    if (computed_values.outline_style() == CSS::OutlineStyle::Auto && (!navigable || !navigable->is_focused()))
        return {};

    return border_data_for_outline(node, computed_values.outline_color(), computed_values.outline_style(), computed_values.outline_width());
}

CSSPixels outline_offset(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return {};
    return as<Layout::NodeWithStyle>(node).outline_offset();
}

CSSPixelRect transform_reference_box(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_transform_reference_box(node.arena_handle(), committed_row_slot(node));
}

CSSPixelRect transform_rect_to_viewport(Layout::Node const& node, CSSPixelRect const& rect, AccumulatedVisualContextTree::IncludeVisualViewportTransform include_visual_viewport_transform)
{
    auto const* row = committed_row(node);
    if (!row)
        return {};
    auto const& document = node.document();
    if (!document.layout_node() || !has_committed_box(*document.layout_node()))
        return rect;
    auto pixel_ratio = static_cast<float>(document.page().client().device_pixels_per_css_pixel());
    auto result = document.visual_context_tree().transform_rect_to_viewport(
        row->accumulated_visual_context.spatial, rect.to_type<float>() * pixel_ratio,
        document.scroll_state_snapshot(), include_visual_viewport_transform);
    return (result * (1.f / pixel_ratio)).to_type<CSSPixels>();
}

Optional<CSSPixelPoint> transform_point_to_local(Layout::Node const& node, CSSPixelPoint position)
{
    auto const* row = committed_row(node);
    if (!row)
        return {};
    auto const& document = node.document();
    if (!document.layout_node() || !has_committed_box(*document.layout_node()))
        return position;
    auto pixel_ratio = static_cast<float>(document.page().client().device_pixels_per_css_pixel());
    auto result = document.visual_context_tree().transform_point_for_hit_test(
        row->accumulated_visual_context, position.to_type<float>() * pixel_ratio,
        document.scroll_state_snapshot());
    if (!result.has_value())
        return {};
    return (*result / pixel_ratio).to_type<CSSPixels>();
}

CSSPixelPoint inverse_transform_point(Layout::Node const& node, CSSPixelPoint position)
{
    auto const* row = committed_row(node);
    if (!row)
        return {};
    auto const& document = node.document();
    if (!document.layout_node() || !has_committed_box(*document.layout_node()))
        return position;
    auto pixel_ratio = static_cast<float>(document.page().client().device_pixels_per_css_pixel());
    auto result = document.visual_context_tree().inverse_transform_point(row->accumulated_visual_context.spatial, position.to_type<float>() * pixel_ratio);
    return (result / pixel_ratio).to_type<CSSPixels>();
}

CSSPixelPoint transform_to_local_coordinates(Layout::Node const& node, CSSPixelPoint position)
{
    if (!has_committed_box(node))
        return {};
    return transform_point_to_local(node, position).value_or(position);
}

Optional<String> grid_layout_json(Layout::Node const& node, UniqueNodeID container_node_id)
{
    Optional<String> result;
    Layout::RustFFI::layout_arena_paintable_grid_layout_json(node.arena_handle(), committed_row_slot(node), container_node_id.value(), &result,
        [](void* context, u8 const* bytes, size_t length) {
            *static_cast<Optional<String>*>(context) = MUST(String::from_utf8(StringView { bytes, length }));
        });
    return result;
}

Optional<String> flex_layout_json(Layout::Node const& node, UniqueNodeID container_node_id)
{
    Optional<String> result;
    Layout::RustFFI::layout_arena_paintable_flex_layout_json(node.arena_handle(), committed_row_slot(node), container_node_id.value(), &result,
        [](void* context, u8 const* bytes, size_t length) {
            *static_cast<Optional<String>*>(context) = MUST(String::from_utf8(StringView { bytes, length }));
        });
    return result;
}

SelectionStyle selection_style_for_node(Layout::Node const& layout_node, GC::Ptr<DOM::Node const> node)
{
    // Selections render in a muted color while the window does not have focus.
    auto navigable = layout_node.document().navigable();
    auto window_is_active = navigable && navigable->is_focused();
    auto const* layout_node_with_style = as_if<Layout::NodeWithStyle>(layout_node);
    auto const& style_source = layout_node_with_style ? *layout_node_with_style : *layout_node.parent();

    auto default_style_for_color_scheme = [&](CSS::PreferredColorScheme color_scheme, bool use_palette_for_normal_color_scheme = true) {
        auto palette = layout_node.document().page().palette();
        auto palette_color_scheme = palette.is_dark() ? CSS::PreferredColorScheme::Dark : CSS::PreferredColorScheme::Light;
        if (color_scheme == palette_color_scheme || use_palette_for_normal_color_scheme) {
            auto background = window_is_active ? palette.selection() : palette.inactive_selection();
            return SelectionStyle { CSS::SystemColor::transform_selection_background_color(background) };
        }

        auto background = window_is_active ? CSS::SystemColor::highlight(color_scheme) : CSS::SystemColor::inactive_highlight(color_scheme);
        return SelectionStyle { CSS::SystemColor::transform_selection_background_color(background) };
    };

    // For text nodes, check the parent element since text nodes don't have computed properties.
    if (!node)
        return default_style_for_color_scheme(style_source.color_scheme());

    DOM::Element const* element = as_if<DOM::Element>(*node);
    if (!element)
        element = node->parent_element().ptr();
    if (!element)
        return default_style_for_color_scheme(style_source.color_scheme());

    auto color_scheme_is_normal = style_source.color_schemes().is_empty();
    auto use_palette_for_normal_color_scheme = color_scheme_is_normal && !layout_node.document().supported_color_schemes().has_value();
    auto default_style = default_style_for_color_scheme(style_source.color_scheme(), use_palette_for_normal_color_scheme);

    auto style_from_element = [&](DOM::Element const& element) -> Optional<SelectionStyle> {
        auto computed_selection_style = element.computed_style(CSS::PseudoElement::Selection);
        if (!computed_selection_style)
            return {};

        SelectionStyle style;
        style.background_color = computed_selection_style->background_color();

        // Only use text color if it was explicitly set in the ::selection rule, not inherited.
        if (!computed_selection_style->is_property_inherited(CSS::PropertyID::Color))
            style.text_color = computed_selection_style->color();

        // Only use text-shadow if it was explicitly set in the ::selection rule, not inherited.
        if (!computed_selection_style->is_property_inherited(CSS::PropertyID::TextShadow)) {
            auto const& css_shadows = computed_selection_style->text_shadow();
            Vector<ShadowData> shadows;
            shadows.ensure_capacity(css_shadows.size());
            for (auto const& shadow : css_shadows)
                shadows.unchecked_append(ShadowData::from_css(shadow));
            style.text_shadow = move(shadows);
        }

        // Only use text-decoration if it was explicitly set in the ::selection rule, not inherited.
        if (!computed_selection_style->is_property_inherited(CSS::PropertyID::TextDecorationLine)) {
            style.text_decoration = TextDecorationStyle {
                .line = Vector<CSS::TextDecorationLine> { computed_selection_style->text_decoration_line() },
                .style = computed_selection_style->text_decoration_style(),
                .color = computed_selection_style->text_decoration_color(),
            };
        }

        // Only return a style if there's a meaningful customization. This allows us to continue checking shadow hosts
        // when the current element only has UA default styles.
        if (!style.has_styling())
            return {};

        return style;
    };

    // Check the element itself.
    if (auto style = style_from_element(*element); style.has_value())
        return style.release_value();

    // If inside a shadow tree, check the shadow host. This enables ::selection styling on elements like <input> to
    // apply to text rendered inside their shadow DOM.
    if (auto shadow_root = element->containing_shadow_root(); shadow_root && shadow_root->is_user_agent_internal()) {
        if (auto const* host = shadow_root->host()) {
            if (auto style = style_from_element(*host); style.has_value())
                return style.release_value();
        }
    }

    return default_style;
}

class BoxViewRepaintAccess {
public:
    static void set_document_needs_repaint(DOM::Document& document, InvalidateDisplayList should_invalidate_display_list)
    {
        document.set_needs_repaint(Badge<BoxViewRepaintAccess> {}, should_invalidate_display_list);
    }
};

void set_needs_repaint(Layout::Node const& node, InvalidateDisplayList should_invalidate_display_list)
{
    if (!has_committed_box(node))
        return;

    auto& document = const_cast<DOM::Document&>(node.document());
    if (should_invalidate_display_list == InvalidateDisplayList::Yes) {
        Layout::RustFFI::layout_arena_paintable_invalidate_for_repaint(node.arena_handle(), committed_row_slot(node));

        // The root element paints the body's propagated background, so a body repaint must also refresh the
        // root's cached background. A root repaint can conversely flip whether propagation applies, changing
        // what the body itself paints.
        if (body_background_is_propagated_to_root(as<Layout::NodeWithStyle>(node))) {
            if (auto const* document_element = document.document_element()) {
                if (auto const* document_element_layout_node = document_element->unsafe_layout_node())
                    invalidate_paint_cache(*document_element_layout_node);
            }
        } else if (node.is_root_element()) {
            if (auto const* body = document.body()) {
                if (auto const* body_layout_node = body->unsafe_layout_node())
                    invalidate_paint_cache(*body_layout_node);
            }
        }
    }
    BoxViewRepaintAccess::set_document_needs_repaint(document, should_invalidate_display_list);
}

void invalidate_paint_cache(Layout::Node const& node)
{
    mirror_rust_invalidate_paint_cache(node);
}

void repaint_after_style_change(Layout::Node const& node, CSS::RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (invalidation.needs_repaint())
        set_needs_repaint(node);
    if (invalidation.repaint_propagated_text_decorations)
        rust_invalidate_propagated_text_decoration_caches(node);
}

void invalidate_stacking_context(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return;
    const_cast<DOM::Document&>(node.document()).paint_state().invalidate_stacking_context_tree();
}

void clear_overflow_data(Layout::Node const& node)
{
    Layout::RustFFI::layout_arena_paintable_clear_overflow_data(node.arena_handle(), committed_row_slot(node));
}

void clear_cached_overflow_data(Layout::Node const& node)
{
    Layout::RustFFI::layout_arena_paintable_clear_cached_overflow_data(node.arena_handle(), committed_row_slot(node));
}

void set_sticky_insets(Layout::Node const& node, OwnPtr<StickyInsets> sticky_insets)
{
    Layout::RustFFI::FfiStickyInsets ffi_insets {};
    if (sticky_insets) {
        auto pack = [](Optional<CSSPixels> const& value, CSSPixels& output, bool& present) {
            present = value.has_value();
            output = value.value_or({});
        };
        pack(sticky_insets->top, ffi_insets.top, ffi_insets.has_top);
        pack(sticky_insets->right, ffi_insets.right, ffi_insets.has_right);
        pack(sticky_insets->bottom, ffi_insets.bottom, ffi_insets.has_bottom);
        pack(sticky_insets->left, ffi_insets.left, ffi_insets.has_left);
    }
    Layout::RustFFI::layout_arena_paintable_set_sticky_insets(node.arena_handle(), committed_row_slot(node), ffi_insets, !!sticky_insets);
}

void inline_piece_border_box_rects(Layout::Node const& node, Vector<CSSPixelRect>& rects)
{
    Layout::RustFFI::layout_arena_inline_paintable_piece_border_box_rects(
        node.arena_handle(), committed_row_slot(node), &rects,
        [](void* context, CSSPixelRect rect) {
            static_cast<Vector<CSSPixelRect>*>(context)->append(rect);
        });
}

CSSPixelPoint cumulative_scroll_compensation(Layout::Node const& node)
{
    auto index = enclosing_scroll_node_index(node);
    if (index == VISUAL_VIEWPORT_NODE_INDEX)
        return {};
    auto const& document = node.document();
    if (!document.layout_node() || !has_committed_box(*document.layout_node()))
        return {};
    auto pixel_ratio = static_cast<float>(document.page().client().device_pixels_per_css_pixel());
    auto device_offset = document.visual_context_tree().cumulative_scroll_chain_offset(index, document.scroll_state_snapshot());
    return { CSSPixels::nearest_value_for(device_offset.x() / pixel_ratio), CSSPixels::nearest_value_for(device_offset.y() / pixel_ratio) };
}

}
