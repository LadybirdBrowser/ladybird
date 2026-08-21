/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Debug.h>
#include <AK/GenericShorthands.h>
#include <AK/Math.h>
#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <AK/ScopeGuard.h>
#include <AK/Utf16StringBuilder.h>
#include <AK/Variant.h>
#include <LibGfx/Path.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Display.h>
#include <LibWeb/CSS/GridTrackPlacement.h>
#include <LibWeb/CSS/GridTrackSize.h>
#include <LibWeb/CSS/Invalidation/ContainerQueryInvalidator.h>
#include <LibWeb/CSS/LengthBox.h>
#include <LibWeb/CSS/StyleValues/AnchorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/ValueType.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/DominantBaseline.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableTypes.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGGeometryElement.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/SVG/SVGTextElement.h>
#include <LibWeb/SVG/SVGTextPathElement.h>
#include <LibWeb/SVG/SVGTextPositioningElement.h>

namespace Web::Layout {

static_assert(to_underlying(CSS::StyleGroupIndex::Count) == RustFFI::STYLE_GROUP_COUNT);
static_assert(to_underlying(CSS::StyleGroupIndex::GridValues) == RustFFI::STYLE_GROUP_INDEX_GRID);
static_assert(to_underlying(CSS::StyleGroupIndex::InheritedTableValues) == RustFFI::STYLE_GROUP_INDEX_INHERITED_TABLE);
static_assert(to_underlying(CSS::StyleGroupIndex::InheritedTextValues) == RustFFI::STYLE_GROUP_INDEX_INHERITED_TEXT);
static_assert(to_underlying(CSS::StyleGroupIndex::InheritedBoxValues) == RustFFI::STYLE_GROUP_INDEX_INHERITED_BOX);
static_assert(to_underlying(CSS::StyleGroupIndex::FontValues) == RustFFI::STYLE_GROUP_INDEX_FONT);
static_assert(to_underlying(CSS::StyleGroupIndex::SVGResetValues) == RustFFI::STYLE_GROUP_INDEX_SVG_RESET);
static_assert(to_underlying(CSS::StyleGroupIndex::BorderValues) == RustFFI::STYLE_GROUP_INDEX_BORDER);
static_assert(to_underlying(CSS::StyleGroupIndex::AlignmentValues) == RustFFI::STYLE_GROUP_INDEX_ALIGNMENT);
static_assert(to_underlying(CSS::StyleGroupIndex::SizingValues) == RustFFI::STYLE_GROUP_INDEX_SIZING);
static_assert(to_underlying(CSS::StyleGroupIndex::SurroundValues) == RustFFI::STYLE_GROUP_INDEX_SURROUND);
static_assert(to_underlying(CSS::StyleGroupIndex::BoxValues) == RustFFI::STYLE_GROUP_INDEX_BOX);

Painting::UsedGridTrackList build_used_grid_track_list(RustFFI::FfiUsedGridTrackList const& list)
{
    VERIFY(list.is_subgrid ? list.track_count == 0 : list.track_count + 1 == list.line_count);

    Painting::UsedGridTrackList result;
    result.is_subgrid = list.is_subgrid;
    result.lines.ensure_capacity(list.line_count);
    result.track_sizes.ensure_capacity(list.track_count);
    for (size_t line_index = 0; line_index < list.line_count; ++line_index) {
        CSS::GridLineNames line_names;
        auto const& line = list.lines[line_index];
        for (size_t name_index = 0; name_index < line.name_count; ++name_index)
            line_names.append(Utf16FlyString::from_raw(line.names[name_index]));
        result.lines.unchecked_append(move(line_names));

        if (line_index < list.track_count)
            result.track_sizes.unchecked_append(CSSPixels::from_raw(list.track_sizes[line_index]));
    }
    return result;
}

static RustFFI::FfiAffineTransform to_ffi_affine_transform(Gfx::AffineTransform const& transform)
{
    return {
        .a = transform.a(),
        .b = transform.b(),
        .c = transform.c(),
        .d = transform.d(),
        .e = transform.e(),
        .f = transform.f(),
    };
}

static RustFFI::FfiSvgViewBox to_ffi_svg_view_box(SVG::ViewBox const& view_box)
{
    return {
        .min_x = view_box.min_x,
        .min_y = view_box.min_y,
        .width = view_box.width,
        .height = view_box.height,
    };
}

static RustFFI::FfiSvgElementFacts build_svg_element_facts(NodeWithStyle const& node)
{
    auto const* dom_node = node.dom_node();
    if (!dom_node)
        return {};

    Optional<SVG::ViewBox> active_view_box;
    if (auto const* svg_graphics_element = as_if<SVG::SVGGraphicsElement>(*dom_node))
        active_view_box = svg_graphics_element->active_view_box();
    else if (auto const* svg_fit_to_view_box = as_if<SVG::SVGFitToViewBox>(*dom_node))
        active_view_box = svg_fit_to_view_box->view_box();

    SVG::PreserveAspectRatio preserve_aspect_ratio {};
    if (auto const* fit_to_view_box = as_if<SVG::SVGFitToViewBox>(*dom_node))
        preserve_aspect_ratio = fit_to_view_box->preserve_aspect_ratio().value_or(SVG::PreserveAspectRatio {});
    else if (is<SVG::SVGMaskElement>(*dom_node) || is<SVG::SVGClipPathElement>(*dom_node))
        preserve_aspect_ratio = { SVG::PreserveAspectRatio::Align::None, {} };

    Gfx::AffineTransform element_transform;
    float visible_stroke_width = 0;
    if (auto const* graphics_element = as_if<SVG::SVGGraphicsElement>(*dom_node)) {
        element_transform = node.used_svg_element_transform();
        visible_stroke_width = graphics_element->visible_stroke_width();
    }

    SVG::SVGUnits content_units {};
    SVG::SVGUnits pattern_units {};
    SVG::NumberPercentage pattern_width = SVG::NumberPercentage::create_number(0);
    SVG::NumberPercentage pattern_height = SVG::NumberPercentage::create_number(0);
    if (node.is_svg_mask_box())
        content_units = as<SVG::SVGMaskElement>(*node.dom_node()).mask_content_units();
    else if (node.is_svg_clip_box())
        content_units = as<SVG::SVGClipPathElement>(*node.dom_node()).clip_path_units();
    else if (node.is_svg_pattern_box()) {
        auto const& pattern_element = as<SVG::SVGPatternElement>(*node.dom_node());
        content_units = pattern_element.pattern_content_units();
        pattern_units = pattern_element.pattern_units();
        pattern_width = pattern_element.pattern_width();
        pattern_height = pattern_element.pattern_height();
    }

    return {
        .is_document_element = node.document().document_element() == dom_node,
        .document_is_decoded_svg = node.document().is_decoded_svg(),
        .is_fit_to_view_box = is<SVG::SVGFitToViewBox>(*dom_node),
        .has_active_view_box = active_view_box.has_value(),
        .active_view_box = active_view_box.has_value() ? to_ffi_svg_view_box(*active_view_box) : RustFFI::FfiSvgViewBox {},
        .preserve_aspect_ratio_align = static_cast<u8>(to_underlying(preserve_aspect_ratio.align)),
        .preserve_aspect_ratio_meet_or_slice = static_cast<u8>(to_underlying(preserve_aspect_ratio.meet_or_slice)),
        .element_transform = to_ffi_affine_transform(element_transform),
        .visible_stroke_width = visible_stroke_width,
        .content_units = static_cast<u8>(to_underlying(content_units)),
        .pattern_units = static_cast<u8>(to_underlying(pattern_units)),
        .pattern_width = {
            .value = pattern_width.value(),
            .is_percentage = pattern_width.is_percentage(),
        },
        .pattern_height = {
            .value = pattern_height.value(),
            .is_percentage = pattern_height.is_percentage(),
        },
    };
}

static Utf16String rendered_svg_text_contents(SVG::SVGTextContentElement const& element)
{
    Utf16StringBuilder builder;
    element.for_each_in_subtree_of_type<DOM::Text>([&](auto const& text_node) {
        if (text_node.parent() && text_node.parent()->unsafe_layout_node()) {
            if (auto content = text_node.text_content(); content.has_value())
                builder.append(*content);
        }
        return TraversalDecision::Continue;
    });
    return builder.to_string().trim_ascii_whitespace();
}

// The advance of the text run rendered by the given box; that is, of its direct child text content.
static float svg_text_run_advance(Box const& text_box)
{
    // FIXME: Use per-code-point fonts.
    return text_box.first_available_font().width(static_cast<SVG::SVGTextContentElement const&>(*text_box.dom_node()).text_contents());
}

// https://svgwg.org/svg2-draft/text.html#TermTextChunk
// Each new absolute positioning adjustment (due to an 'x' or 'y' attribute, or forced line break) creates a new text chunk.
// https://svgwg.org/svg2-draft/text.html#TextElementXAttribute
// NB: The initial value of 'x' and 'y' is "0 for 'text'; (none) for 'tspan'". So, a <text> element always positions its
//     first character absolutely, and so always starts a chunk.
static bool svg_text_box_starts_text_chunk(Box const& text_box)
{
    if (is<SVG::SVGTextElement>(*text_box.dom_node()))
        return true;
    auto text_positioning = as<SVG::SVGTextPositioningElement>(*text_box.dom_node()).text_positioning();
    return !text_positioning.x.is_empty() || !text_positioning.y.is_empty();
}

struct SvgTextChunkMeasurement {
    float advance { 0 };
    CSS::TextAnchor anchor { CSS::TextAnchor::Start };
};

// Measures the total advance of the text chunk that starts at the given box, and determines the 'text-anchor' value
// that applies to the chunk. The chunk extends in document order through the subtree of the containing <text> element
// until the next box that starts a chunk of its own.
static SvgTextChunkMeasurement measure_svg_text_chunk(Box const& chunk_start_box)
{
    auto const* subtree_root = &chunk_start_box;
    for (auto const* ancestor = chunk_start_box.parent(); ancestor && ancestor->kind() == RustFFI::NodeKind::SVGTextBox; ancestor = ancestor->parent())
        subtree_root = static_cast<Box const*>(ancestor);

    SvgTextChunkMeasurement measurement;
    bool found_chunk_start = false;
    bool found_first_rendered_text = false;
    subtree_root->for_each_in_inclusive_subtree([&](Node const& node) {
        // AD-HOC: Text on a path is laid out independently; see compute_path_for_svg_text_path().
        if (node.kind() == RustFFI::NodeKind::SVGTextPathBox)
            return TraversalDecision::SkipChildrenAndContinue;
        if (node.kind() != RustFFI::NodeKind::SVGTextBox)
            return TraversalDecision::Continue;
        auto const* text_box = static_cast<Box const*>(&node);
        if (text_box == &chunk_start_box)
            found_chunk_start = true;
        else if (found_chunk_start && svg_text_box_starts_text_chunk(*text_box))
            return TraversalDecision::Break;
        if (found_chunk_start && !static_cast<SVG::SVGTextContentElement const&>(*text_box->dom_node()).text_contents().is_empty()) {
            if (!found_first_rendered_text) {
                // https://svgwg.org/svg2-draft/text.html#TextLayoutAlgorithm
                // Adjust shift based on the value of 'text-anchor' and 'direction' of the element the character at index i.
                // FIXME: Take text direction into account.
                measurement.anchor = text_box->text_anchor();
                found_first_rendered_text = true;
            }
            measurement.advance += svg_text_run_advance(*text_box);
        }
        return TraversalDecision::Continue;
    });
    return measurement;
}

static Gfx::Path compute_path_for_svg_text(Box const& text_box, Gfx::FloatPoint current_text_position)
{
    auto const& text_element = static_cast<SVG::SVGTextContentElement const&>(*text_box.dom_node());
    // FIXME: Use per-code-point fonts.
    auto& font = text_box.first_available_font();
    auto text_contents = text_element.text_contents();

    auto text_offset = current_text_position;
    auto baseline_metric = resolve_dominant_baseline_metric(text_box);
    text_offset.translate_by(0, dominant_baseline_offset(baseline_metric, font.pixel_metrics()));

    Gfx::Path path;
    path.move_to(text_offset);
    path.text(text_contents, font);
    return path;
}

static Gfx::Path compute_path_for_svg_text_path(Box const& text_path_box, CSSPixelSize viewport_size)
{
    auto const& text_path_element = as<SVG::SVGTextPathElement>(*text_path_box.dom_node());
    auto path_or_shape = text_path_element.path_or_shape();
    if (!path_or_shape)
        return {};

    // FIXME: Use per-code-point fonts.
    auto& font = text_path_box.first_available_font();
    auto text_contents = rendered_svg_text_contents(text_path_element);

    auto shape_path = const_cast<SVG::SVGGeometryElement&>(*path_or_shape).get_path(viewport_size);
    auto start_offset = text_path_element.start_offset_for_path_length(shape_path.length());

    // FIXME: Take writing mode and text direction into account.
    auto total_advance = font.width(text_contents);
    switch (text_path_element.text_anchor().value_or(SVG::TextAnchor::Start)) {
    case SVG::TextAnchor::Start:
        break;
    case SVG::TextAnchor::Middle:
        start_offset -= total_advance / 2;
        break;
    case SVG::TextAnchor::End:
        start_offset -= total_advance;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    return shape_path.place_text_along(text_contents, font, start_offset);
}

static RustFFI::FfiSvgPathResult compute_svg_path(NodeWithStyle const& node, RustFFI::FfiSvgPathRequest const& request)
{
    auto const& graphics_box = as<Box>(node);
    CSSPixelSize viewport_size {
        CSSPixels::from_raw(request.viewport_width),
        CSSPixels::from_raw(request.viewport_height),
    };
    Gfx::FloatPoint text_position {
        request.current_text_position.x,
        request.current_text_position.y,
    };

    Gfx::Path path;
    if (graphics_box.is_svg_geometry_box()) {
        path = as<SVG::SVGGeometryElement>(const_cast<DOM::Node&>(*graphics_box.dom_node())).get_path(viewport_size);
    } else if (graphics_box.kind() == RustFFI::NodeKind::SVGTextBox) {
        auto const* text_box = &graphics_box;
        auto const& text_element = as<SVG::SVGTextPositioningElement>(*text_box->dom_node());
        // https://svgwg.org/svg2-draft/text.html#TextElementXAttribute
        // the starting X (Y) coordinate for rendering the glyphs corresponding to the given character is the X (Y) coordinate
        // of the resulting current text position from the most recently rendered glyph for the current 'text' element.
        // NB: The initial value of 'x' and 'y' is "0 for 'text'; (none) for 'tspan'": a <text> element starts at (0, 0)
        //     regardless of the current text position, while a <tspan> without 'x'/'y' continues at the current text position.
        if (is<SVG::SVGTextElement>(text_element))
            text_position = {};
        text_element.text_positioning().apply_to_text_position(viewport_size, text_position, 0u);
        if (svg_text_box_starts_text_chunk(*text_box)) {
            // https://svgwg.org/svg2-draft/text.html#TextAnchoringProperties
            // The 'text-anchor' property is applied to each individual text chunk within a given 'text' element.
            // AD-HOC: The spec applies 'text-anchor' as a shift of the chunk's rendered glyphs after layout; shifting
            //         the chunk's starting position up front by the chunk's total advance is equivalent for horizontal
            //         text — since every run in the chunk is laid out sequentially from this position.
            auto chunk = measure_svg_text_chunk(*text_box);
            switch (chunk.anchor) {
            case CSS::TextAnchor::Start:
                // The rendered characters are aligned such that the start of the resulting rendered text is at the
                // initial current text position."
                break;
            case CSS::TextAnchor::Middle:
                // The rendered characters are shifted such that the geometric middle of the resulting rendered text
                // (determined from the initial and final current text position before applying the 'text-anchor'
                // property) is at the initial current text position.
                text_position.translate_by(-chunk.advance / 2, 0);
                break;
            case CSS::TextAnchor::End:
                // The rendered characters are shifted such that the end of the resulting rendered text (final current
                // text position before applying the 'text-anchor' property) is at the initial current text position."
                text_position.translate_by(-chunk.advance, 0);
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        }
        path = compute_path_for_svg_text(*text_box, text_position);
        // https://svgwg.org/svg2-draft/text.html#TextLayoutIntroduction
        // After each glyph is placed, the current text position is advanced by the glyph's advance value (typically the
        // width for horizontal text or height for vertical text).
        // FIXME: Take writing mode and text direction into account.
        text_position.translate_by(svg_text_run_advance(*text_box), 0);
    } else if (graphics_box.kind() == RustFFI::NodeKind::SVGTextPathBox) {
        path = compute_path_for_svg_text_path(graphics_box, viewport_size);
    }

    auto bounding_box = path.bounding_box();
    // Rust adopts this heap-allocated path and destroys it via ladybird_gfx_path_destroy().
    auto* path_handle = new Gfx::Path(move(path));
    return {
        .path_handle = path_handle,
        .bounding_box = {
            .x = bounding_box.x(),
            .y = bounding_box.y(),
            .width = bounding_box.width(),
            .height = bounding_box.height(),
        },
        .text_position_after = {
            .x = text_position.x(),
            .y = text_position.y(),
        },
    };
}

Optional<RustFFI::FfiFormattingContextType> formatting_context_type_created_by_box(Box const& box)
{
    auto type = RustFFI::rust_layout_formatting_context_type_for_box({
        .arena = box.arena_handle(),
        .node = Node::slot_id(&box),
    });
    if (type == NumericLimits<u8>::max())
        return {};
    return static_cast<RustFFI::FfiFormattingContextType>(type);
}

StringView formatting_context_type_name(RustFFI::FfiFormattingContextType type)
{
    switch (type) {
    case RustFFI::FfiFormattingContextType::Block:
        return "BFC"sv;
    case RustFFI::FfiFormattingContextType::Inline:
        return "IFC"sv;
    case RustFFI::FfiFormattingContextType::Flex:
        return "FFC"sv;
    case RustFFI::FfiFormattingContextType::Grid:
        return "GFC"sv;
    case RustFFI::FfiFormattingContextType::Table:
        return "TFC"sv;
    case RustFFI::FfiFormattingContextType::Svg:
        return "SVG"sv;
    case RustFFI::FfiFormattingContextType::ReplacedWithChildren:
        return "Replaced, with children"sv;
    case RustFFI::FfiFormattingContextType::InternalReplaced:
        return "Replaced"sv;
    case RustFFI::FfiFormattingContextType::InternalDummy:
        return "Dummy"sv;
    }
    VERIFY_NOT_REACHED();
}

static size_t s_active_layout_pass_count { 0 };

bool layout_pass_currently_running()
{
    return s_active_layout_pass_count > 0;
}

class ActiveLayoutPassScope {
public:
    ActiveLayoutPassScope() { ++s_active_layout_pass_count; }
    ~ActiveLayoutPassScope() { --s_active_layout_pass_count; }
};

LayoutRustBridge::LayoutRustBridge() = default;

LayoutRustBridge::~LayoutRustBridge() = default;

void LayoutRustBridge::run_root_layout(Box& viewport, CSSPixels viewport_inline_size, CSSPixels viewport_block_size, bool should_collect_devtools_layout_data)
{
    VERIFY(!m_commit_root);
    m_commit_root = &viewport;
    ScopeGuard clear_commit_root = [&] {
        m_commit_root = nullptr;
    };

    viewport.document().invalidate_stacking_context_tree();
    auto callbacks = formatting_context_callbacks();
    auto sink = commit_sink();
    {
        ActiveLayoutPassScope active_pass;
        RustFFI::rust_layout_run_root_layout(
            Node::slot_id(&viewport),
            viewport_inline_size.raw_value(),
            viewport_block_size.raw_value(),
            should_collect_devtools_layout_data,
            &callbacks,
            &sink);
    }
}

void LayoutRustBridge::compute_subtree_layout(Box& root)
{
    VERIFY(!m_commit_root);
    m_commit_root = &root;
    ScopeGuard clear_commit_root = [&] {
        m_commit_root = nullptr;
    };

    root.document().invalidate_stacking_context_tree();
    auto viewport_rect = root.document().viewport_rect();
    auto callbacks = formatting_context_callbacks();
    auto sink = commit_sink();
    {
        ActiveLayoutPassScope active_pass;
        RustFFI::rust_layout_compute_subtree_layout(
            Node::slot_id(&root),
            Node::slot_id(&root.root()),
            viewport_rect.width().raw_value(),
            viewport_rect.height().raw_value(),
            &callbacks,
            &sink);
    }
}

void LayoutRustBridge::replay_saved_abspos_layout(Box& box)
{
    VERIFY(!m_commit_root);
    m_commit_root = &box;
    ScopeGuard clear_commit_root = [&] {
        m_commit_root = nullptr;
    };

    box.document().invalidate_stacking_context_tree();
    auto callbacks = formatting_context_callbacks();
    auto sink = commit_sink();
    {
        ActiveLayoutPassScope active_pass;
        RustFFI::rust_layout_replay_saved_abspos_layout(Node::slot_id(&box), &callbacks, &sink);
    }
}

static bool content_size_change_affects_container_queries(NodeWithStyle const& layout_node, CSSPixelSize old_size, CSSPixelSize new_size)
{
    auto container_type = layout_node.container_type();
    if (container_type.is_size_container)
        return old_size != new_size;
    if (!container_type.is_inline_size_container)
        return false;
    if (layout_node.writing_mode() == CSS::WritingMode::HorizontalTb)
        return old_size.width() != new_size.width();
    return old_size.height() != new_size.height();
}

static void invalidate_descendant_styles_for_container_query_size_change(GC::Ptr<DOM::Node> node, CSSPixelSize old_size, CSSPixelSize new_size)
{
    auto* element = as_if<DOM::Element>(node.ptr());
    if (!element)
        return;
    auto const* layout_node = element->unsafe_layout_node();
    if (!layout_node || !content_size_change_affects_container_queries(*layout_node, old_size, new_size))
        return;
    CSS::Invalidation::invalidate_descendant_styles_depending_on_size_container_query(*element);
}

RustFFI::FfiCommitSink LayoutRustBridge::commit_sink()
{
    return {
        .context = this,
        .content_size_changed = [](void*, void* layout_node_shell, RustFFI::FfiCssPixelSize old_size, RustFFI::FfiCssPixelSize new_size) {
            auto& layout_node = *static_cast<Node*>(layout_node_shell);
            invalidate_descendant_styles_for_container_query_size_change(
                layout_node.dom_node(),
                { CSSPixels::from_raw(old_size.width), CSSPixels::from_raw(old_size.height) },
                { CSSPixels::from_raw(new_size.width), CSSPixels::from_raw(new_size.height) }); },
        .finish_commit = [](void*, void* const* viewport_shells, size_t viewport_count) {
            for (size_t index = 0; index < viewport_count; ++index)
                as<Box>(*static_cast<Node*>(viewport_shells[index])).notify_content_navigable_of_committed_viewport(); },
    };
}

static Optional<DOM::AbstractElement> abstract_element_for_abspos_box(Box const& box)
{
    if (box.is_generated_for_pseudo_element())
        return DOM::AbstractElement { *box.pseudo_element_generator(), box.generated_for_pseudo_element() };
    if (auto const* element = as_if<DOM::Element>(box.dom_node()))
        return DOM::AbstractElement { *element };
    return {};
}

bool can_replay_saved_abspos_layout_inputs_after_style_change(Box const& box)
{
    if (!box.containing_block())
        return false;

    if (box.saved_abspos_cb_derives_from_own_computed_values())
        return false;

    auto inset = box.inset();
    bool uses_static_position = (inset.left().is_auto() && inset.right().is_auto())
        || (inset.top().is_auto() && inset.bottom().is_auto());
    if (uses_static_position && box.saved_abspos_alignment_derives_from_own_computed_values())
        return false;

    return true;
}

RustFFI::FfiLayoutFcCallbacks LayoutRustBridge::formatting_context_callbacks()
{
    static_assert(to_underlying(CSS::GridRepeatType::AutoFit) == 0);
    static_assert(to_underlying(CSS::GridRepeatType::AutoFill) == 1);
    static_assert(to_underlying(CSS::GridRepeatType::Fixed) == 2);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::None) == 0);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMinYMin) == 1);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMidYMin) == 2);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMaxYMin) == 3);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMinYMid) == 4);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMidYMid) == 5);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMaxYMid) == 6);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMinYMax) == 7);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMidYMax) == 8);
    static_assert(to_underlying(SVG::PreserveAspectRatio::Align::xMaxYMax) == 9);
    static_assert(to_underlying(SVG::PreserveAspectRatio::MeetOrSlice::Meet) == 0);
    static_assert(to_underlying(SVG::PreserveAspectRatio::MeetOrSlice::Slice) == 1);
    static_assert(to_underlying(SVG::SVGUnits::ObjectBoundingBox) == 0);
    static_assert(to_underlying(SVG::SVGUnits::UserSpaceOnUse) == 1);
    return {
        .context = this,
        .arena = m_commit_root->arena_handle(),
        .initial_containing_block_inline_size = m_commit_root->document().viewport_rect().width().raw_value(),
        .document_in_quirks_mode = m_commit_root->document().in_quirks_mode(),
        .report_unexpected_fragmented_inline = [](void*, void* node) {
            auto const& box = *static_cast<Box const*>(node);
            dbgln("FIXME: InlineFormattingContext::dimension_box_on_line got unexpected box in inline context:");
            dump_tree(box); },
        .build_svg_facts = [](void*, void* node) {
            auto const* node_with_style = as_if<NodeWithStyle>(*static_cast<Node const*>(node));
            VERIFY(node_with_style);
            return build_svg_element_facts(*node_with_style); },
        .compute_svg_path = [](void*, void* node, RustFFI::FfiSvgPathRequest request) {
            auto const* node_with_style = as_if<NodeWithStyle>(*static_cast<Node const*>(node));
            VERIFY(node_with_style);
            return compute_svg_path(*node_with_style, request); },
        .svg_image_bounding_box = [](void*, void* node, i32 viewport_width, i32 viewport_height) {
            auto const& image_element = as<SVG::SVGImageElement>(*static_cast<Node const*>(node)->dom_node());
            auto bounding_box = image_element.bounding_box({
                CSSPixels::from_raw(viewport_width),
                CSSPixels::from_raw(viewport_height),
            });
            return RustFFI::FfiFloatRect {
                .x = bounding_box.x(),
                .y = bounding_box.y(),
                .width = bounding_box.width(),
                .height = bounding_box.height(),
            }; },
        .anchor_lookup = [](void*, void* node, size_t anchor_name, void* const* eligible_anchor_boxes, size_t eligible_anchor_box_count) {
            auto const& box = *static_cast<Box const*>(node);
            auto abstract_element = abstract_element_for_abspos_box(box);
            if (!abstract_element.has_value())
                return RustFFI::NodeSlotId_INVALID;
            auto const* containing_block = box.containing_block();
            if (!containing_block)
                return RustFFI::NodeSlotId_INVALID;
            Function<bool(DOM::Element&)> is_acceptable_anchor_element = [&](DOM::Element& candidate) {
                auto const* anchor_box = as_if<Box>(candidate.unsafe_layout_node());
                if (!anchor_box || anchor_box == &box)
                    return false;
                bool has_used_values = false;
                for (size_t index = 0; index < eligible_anchor_box_count; ++index) {
                    if (eligible_anchor_boxes[index] == anchor_box) {
                        has_used_values = true;
                        break;
                    }
                }
                if (!has_used_values)
                    return false;
                for (auto const* ancestor = anchor_box->containing_block(); ancestor; ancestor = ancestor->containing_block()) {
                    if (ancestor == containing_block)
                        return true;
                }
                return false;
            };
            auto anchor_element = abstract_element->element().document().element_by_anchor_name(
                Utf16FlyString::from_raw(anchor_name),
                abstract_element->element(),
                is_acceptable_anchor_element);
            if (!anchor_element)
                return RustFFI::NodeSlotId_INVALID;
            auto const* anchor_box = as_if<Box>(anchor_element->unsafe_layout_node());
            if (!anchor_box)
                return RustFFI::NodeSlotId_INVALID;
            return Node::slot_id(anchor_box); },
        .node_unique_id = [](void* node) -> i64 {
            auto const* dom_node = static_cast<Box const*>(node)->dom_node();
            return dom_node ? dom_node->unique_id().value() : -1;
        },
        .set_default_scroll_shift = [](void*, void* node, void* anchor, bool horizontal, bool vertical) {
            auto& box = *static_cast<Box*>(node);
            if (!anchor) {
                box.set_default_scroll_shift({}, false, false);
                return;
            }
            box.set_default_scroll_shift(static_cast<Box*>(anchor)->make_weak_ptr(), horizontal, vertical); },
    };
}

}

extern "C" WEB_API u8 ladybird_layout_text_type_for_code_point(u32 code_point)
{
    return static_cast<u8>(to_underlying(Web::Layout::text_type_for_code_point(code_point)));
}

extern "C" WEB_API bool ladybird_layout_code_point_has_break_all_line_break_class(u32 code_point)
{
    return first_is_one_of(Unicode::line_break_class(code_point),
        Unicode::LineBreakClass::Alphabetic,
        Unicode::LineBreakClass::Numeric,
        Unicode::LineBreakClass::ComplexContext,
        Unicode::LineBreakClass::Ideographic);
}

extern "C" WEB_API bool ladybird_layout_code_point_has_keep_all_line_break_class(u32 code_point)
{
    return first_is_one_of(Unicode::line_break_class(code_point),
        Unicode::LineBreakClass::Alphabetic,
        Unicode::LineBreakClass::Numeric,
        Unicode::LineBreakClass::Ambiguous,
        Unicode::LineBreakClass::Ideographic);
}

extern "C" WEB_API bool ladybird_layout_code_point_has_combining_mark_line_break_class(u32 code_point)
{
    return Unicode::line_break_class(code_point) == Unicode::LineBreakClass::CombiningMark;
}

extern "C" WEB_API bool ladybird_layout_code_point_has_emoji_property(u32 code_point)
{
    return Unicode::code_point_has_emoji_property(code_point);
}

extern "C" WEB_API size_t ladybird_layout_text_node_dom_offset_for_rendered_text_offset(void* node, size_t offset, bool use_end_boundary)
{
    auto const& layout_node = *static_cast<Web::Layout::Node*>(node);
    VERIFY(is<Web::Layout::TextNode>(layout_node));
    auto const& text_node = static_cast<Web::Layout::TextNode const&>(layout_node);
    auto boundary = use_end_boundary
        ? Web::Layout::TextNode::RenderedTextBoundary::End
        : Web::Layout::TextNode::RenderedTextBoundary::Start;
    return text_node.dom_offset_for_rendered_text_offset(offset, boundary);
}

extern "C" WEB_API size_t ladybird_layout_text_node_rendered_text_offset_for_dom_offset(void* node, size_t offset, bool use_end_boundary)
{
    auto const& layout_node = *static_cast<Web::Layout::Node*>(node);
    VERIFY(is<Web::Layout::TextNode>(layout_node));
    auto const& text_node = static_cast<Web::Layout::TextNode const&>(layout_node);
    auto boundary = use_end_boundary
        ? Web::Layout::TextNode::RenderedTextBoundary::End
        : Web::Layout::TextNode::RenderedTextBoundary::Start;
    return text_node.rendered_text_offset_for_dom_offset(offset, boundary);
}
