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
#include <LibGfx/Font/Font.h>
#include <LibGfx/Path.h>
#include <LibGfx/TextLayout.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Display.h>
#include <LibWeb/CSS/GridTrackPlacement.h>
#include <LibWeb/CSS/GridTrackSize.h>
#include <LibWeb/CSS/LengthBox.h>
#include <LibWeb/CSS/Size.h>
#include <LibWeb/CSS/StyleValues/AnchorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
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
#include <LibWeb/Layout/FieldSetBox.h>
#include <LibWeb/Layout/FlexLayoutData.h>
#include <LibWeb/Layout/GridLayoutData.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/SVGClipBox.h>
#include <LibWeb/Layout/SVGGeometryBox.h>
#include <LibWeb/Layout/SVGImageBox.h>
#include <LibWeb/Layout/SVGMaskBox.h>
#include <LibWeb/Layout/SVGPatternBox.h>
#include <LibWeb/Layout/SVGSVGBox.h>
#include <LibWeb/Layout/SVGTextBox.h>
#include <LibWeb/Layout/SVGTextPathBox.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/SVGForeignObjectPaintable.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>
#include <LibWeb/Painting/SVGPathPaintable.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/SVG/SVGTextElement.h>

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

static CSS::GridTrackSizeList build_used_grid_track_list(RustFFI::FfiUsedGridTrackList const& list)
{
    auto result = list.is_subgrid ? CSS::GridTrackSizeList::make_subgrid() : CSS::GridTrackSizeList::make_none();
    VERIFY(list.is_subgrid ? list.track_count == 0 : list.track_count + 1 == list.line_count);

    for (size_t line_index = 0; line_index < list.line_count; ++line_index) {
        CSS::GridLineNames line_names;
        auto const& line = list.lines[line_index];
        for (size_t name_index = 0; name_index < line.name_count; ++name_index)
            line_names.append(Utf16FlyString::from_raw(line.names[name_index]));
        if (list.is_subgrid || !line_names.is_empty())
            result.append(move(line_names));

        if (line_index < list.track_count) {
            auto size = CSSPixels::from_raw(list.track_sizes[line_index]);
            result.append(CSS::ExplicitGridTrack {
                CSS::GridSize { CSS::LengthStyleValue::create(CSS::Length::make_px(size)) },
            });
        }
    }
    return result;
}

static OwnPtr<FlexLayoutData> build_flex_layout_data(RustFFI::FfiFlexLayoutData const& ffi_data)
{
    auto growth_state = [](RustFFI::FfiFlexLayoutGrowthState state) {
        switch (state) {
        case RustFFI::FfiFlexLayoutGrowthState::Growing:
            return FlexLayoutGrowthState::Growing;
        case RustFFI::FfiFlexLayoutGrowthState::Shrinking:
            return FlexLayoutGrowthState::Shrinking;
        }
        VERIFY_NOT_REACHED();
    };
    auto clamp_state = [](RustFFI::FfiFlexLayoutClampState state) {
        switch (state) {
        case RustFFI::FfiFlexLayoutClampState::Unclamped:
            return FlexLayoutClampState::Unclamped;
        case RustFFI::FfiFlexLayoutClampState::ClampedToMin:
            return FlexLayoutClampState::ClampedToMin;
        case RustFFI::FfiFlexLayoutClampState::ClampedToMax:
            return FlexLayoutClampState::ClampedToMax;
        }
        VERIFY_NOT_REACHED();
    };

    auto data = make<FlexLayoutData>();
    data->align_content = static_cast<CSS::AlignContent>(ffi_data.align_content);
    data->align_items = static_cast<CSS::AlignItems>(ffi_data.align_items);
    data->flex_direction = static_cast<CSS::FlexDirection>(ffi_data.flex_direction);
    data->flex_wrap = static_cast<CSS::FlexWrap>(ffi_data.flex_wrap);
    data->justify_content = static_cast<CSS::JustifyContent>(ffi_data.justify_content);

    auto axis_direction = [](u8 direction) -> String {
        switch (direction) {
        case 0:
            return "horizontal-lr"_string;
        case 1:
            return "horizontal-rl"_string;
        case 2:
            return "vertical-tb"_string;
        case 3:
            return "vertical-bt"_string;
        default:
            VERIFY_NOT_REACHED();
        }
    };
    auto main_axis_direction = axis_direction(ffi_data.main_axis_direction);
    auto cross_axis_direction = axis_direction(ffi_data.cross_axis_direction);
    bool main_axis_is_horizontal = ffi_data.main_axis_direction <= 1;

    for (size_t line_index = 0; line_index < ffi_data.line_count; ++line_index) {
        auto const& ffi_line = ffi_data.lines[line_index];
        FlexLayoutLine line;
        line.growth_state = growth_state(ffi_line.growth_state);
        line.cross_start = CSSPixels::from_raw(ffi_line.cross_start);
        line.cross_size = CSSPixels::from_raw(ffi_line.cross_size);
        for (size_t item_index = 0; item_index < ffi_line.item_count; ++item_index) {
            auto const& ffi_item = ffi_line.items[item_index];
            auto const& item_box = *static_cast<Box const*>(ffi_item.node);
            auto flex_basis = item_box.flex_basis();
            auto const& main_size = main_axis_is_horizontal ? item_box.width() : item_box.height();
            auto const& main_min_size = main_axis_is_horizontal ? item_box.min_width() : item_box.min_height();
            auto const& main_max_size = main_axis_is_horizontal ? item_box.max_width() : item_box.max_height();

            FlexLayoutItem item;
            if (auto* dom_node = item_box.dom_node())
                item.node_id = dom_node->unique_id();
            item.main_axis_direction = main_axis_direction;
            item.cross_axis_direction = cross_axis_direction;
            item.rect = {
                CSSPixels::from_raw(ffi_item.rect.x),
                CSSPixels::from_raw(ffi_item.rect.y),
                CSSPixels::from_raw(ffi_item.rect.width),
                CSSPixels::from_raw(ffi_item.rect.height),
            };
            item.main_base_size = CSSPixels::from_raw(ffi_item.main_base_size);
            item.main_delta_size = CSSPixels::from_raw(ffi_item.main_delta_size);
            item.main_min_size = CSSPixels::from_raw(ffi_item.main_min_size);
            item.main_max_size = CSSPixels::from_raw(ffi_item.main_max_size);
            item.cross_min_size = CSSPixels::from_raw(ffi_item.cross_min_size);
            item.cross_max_size = CSSPixels::from_raw(ffi_item.cross_max_size);
            item.clamp_state = clamp_state(ffi_item.clamp_state);
            item.flex_basis = flex_basis.has<CSS::FlexBasisContent>()
                ? "content"_string
                : MUST(String::formatted("{}", flex_basis.get<CSS::Size>()));
            item.main_size_property = MUST(String::formatted("{}", main_size));
            item.main_min_size_property = MUST(String::formatted("{}", main_min_size));
            item.main_max_size_property = MUST(String::formatted("{}", main_max_size));
            item.flex_grow = ffi_item.flex_grow;
            item.flex_shrink = ffi_item.flex_shrink;
            line.items.append(move(item));
        }
        data->lines.append(move(line));
    }
    return data;
}

static OwnPtr<GridLayoutData> build_grid_layout_data(RustFFI::FfiGridLayoutData const& ffi_data)
{
    auto track_type = [](RustFFI::FfiGridTrackType type) {
        switch (type) {
        case RustFFI::FfiGridTrackType::Explicit:
            return GridTrackType::Explicit;
        case RustFFI::FfiGridTrackType::Implicit:
            return GridTrackType::Implicit;
        }
        VERIFY_NOT_REACHED();
    };
    auto track_state = [](RustFFI::FfiGridTrackState state) {
        switch (state) {
        case RustFFI::FfiGridTrackState::Static:
            return GridTrackState::Static;
        case RustFFI::FfiGridTrackState::Repeat:
            return GridTrackState::Repeat;
        case RustFFI::FfiGridTrackState::Removed:
            return GridTrackState::Removed;
        }
        VERIFY_NOT_REACHED();
    };

    auto data = make<GridLayoutData>();
    data->direction = static_cast<CSS::Direction>(ffi_data.direction);
    data->writing_mode = static_cast<CSS::WritingMode>(ffi_data.writing_mode);
    data->is_subgrid = ffi_data.is_subgrid;

    auto build_dimension = [track_type, track_state](RustFFI::FfiGridLayoutDimension const& ffi_dimension) {
        GridLayoutDimension dimension;
        dimension.lines.ensure_capacity(ffi_dimension.line_count);
        for (size_t line_index = 0; line_index < ffi_dimension.line_count; ++line_index) {
            auto const& ffi_line = ffi_dimension.lines[line_index];
            GridLayoutLine line {
                .names = {},
                .start = CSSPixels::from_raw(ffi_line.start),
                .breadth = CSSPixels::from_raw(ffi_line.breadth),
                .type = track_type(ffi_line.type_),
                .number = ffi_line.number,
                .negative_number = ffi_line.negative_number,
            };
            line.names.ensure_capacity(ffi_line.name_count);
            for (size_t name_index = 0; name_index < ffi_line.name_count; ++name_index)
                line.names.unchecked_append(Utf16FlyString::from_raw(ffi_line.names[name_index]));
            dimension.lines.unchecked_append(move(line));
        }

        dimension.tracks.ensure_capacity(ffi_dimension.track_count);
        for (size_t track_index = 0; track_index < ffi_dimension.track_count; ++track_index) {
            auto const& ffi_track = ffi_dimension.tracks[track_index];
            dimension.tracks.unchecked_append({
                .start = CSSPixels::from_raw(ffi_track.start),
                .breadth = CSSPixels::from_raw(ffi_track.breadth),
                .type = track_type(ffi_track.type_),
                .state = track_state(ffi_track.state),
            });
        }
        return dimension;
    };

    data->fragments.ensure_capacity(ffi_data.fragment_count);
    for (size_t fragment_index = 0; fragment_index < ffi_data.fragment_count; ++fragment_index) {
        auto const& ffi_fragment = ffi_data.fragments[fragment_index];
        GridLayoutFragment fragment {
            .areas = {},
            .columns = build_dimension(ffi_fragment.columns),
            .rows = build_dimension(ffi_fragment.rows),
        };
        fragment.areas.ensure_capacity(ffi_fragment.area_count);
        for (size_t area_index = 0; area_index < ffi_fragment.area_count; ++area_index) {
            auto const& ffi_area = ffi_fragment.areas[area_index];
            fragment.areas.unchecked_append({
                .name = Utf16FlyString::from_raw(ffi_area.name),
                .type = track_type(ffi_area.type_),
                .row_start = ffi_area.row_start,
                .row_end = ffi_area.row_end,
                .column_start = ffi_area.column_start,
                .column_end = ffi_area.column_end,
            });
        }
        data->fragments.unchecked_append(move(fragment));
    }
    return data;
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

static Gfx::AffineTransform from_ffi_affine_transform(RustFFI::FfiAffineTransform const& transform)
{
    return {
        transform.a,
        transform.b,
        transform.c,
        transform.d,
        transform.e,
        transform.f,
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
    if (auto const* mask_box = as_if<SVGMaskBox>(node))
        content_units = mask_box->dom_node().mask_content_units();
    else if (auto const* clip_box = as_if<SVGClipBox>(node))
        content_units = clip_box->dom_node().clip_path_units();
    else if (auto const* pattern_box = as_if<SVGPatternBox>(node)) {
        content_units = pattern_box->dom_node().pattern_content_units();
        pattern_units = pattern_box->dom_node().pattern_units();
        pattern_width = pattern_box->dom_node().pattern_width();
        pattern_height = pattern_box->dom_node().pattern_height();
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
static float svg_text_run_advance(SVGTextBox const& text_box)
{
    // FIXME: Use per-code-point fonts.
    return text_box.first_available_font().width(text_box.dom_node().text_contents());
}

// https://svgwg.org/svg2-draft/text.html#TermTextChunk
// Each new absolute positioning adjustment (due to an 'x' or 'y' attribute, or forced line break) creates a new text chunk.
// https://svgwg.org/svg2-draft/text.html#TextElementXAttribute
// NB: The initial value of 'x' and 'y' is "0 for 'text'; (none) for 'tspan'". So, a <text> element always positions its
//     first character absolutely, and so always starts a chunk.
static bool svg_text_box_starts_text_chunk(SVGTextBox const& text_box)
{
    if (is<SVG::SVGTextElement>(text_box.dom_node()))
        return true;
    auto text_positioning = text_box.dom_node().text_positioning();
    return !text_positioning.x.is_empty() || !text_positioning.y.is_empty();
}

struct SvgTextChunkMeasurement {
    float advance { 0 };
    CSS::TextAnchor anchor { CSS::TextAnchor::Start };
};

// Measures the total advance of the text chunk that starts at the given box, and determines the 'text-anchor' value
// that applies to the chunk. The chunk extends in document order through the subtree of the containing <text> element
// until the next box that starts a chunk of its own.
static SvgTextChunkMeasurement measure_svg_text_chunk(SVGTextBox const& chunk_start_box)
{
    auto const* subtree_root = &chunk_start_box;
    for (auto const* ancestor = chunk_start_box.parent(); ancestor && is<SVGTextBox>(*ancestor); ancestor = ancestor->parent())
        subtree_root = static_cast<SVGTextBox const*>(ancestor);

    SvgTextChunkMeasurement measurement;
    bool found_chunk_start = false;
    bool found_first_rendered_text = false;
    subtree_root->for_each_in_inclusive_subtree([&](Node const& node) {
        // AD-HOC: Text on a path is laid out independently; see compute_path_for_svg_text_path().
        if (is<SVGTextPathBox>(node))
            return TraversalDecision::SkipChildrenAndContinue;
        auto const* text_box = as_if<SVGTextBox>(node);
        if (!text_box)
            return TraversalDecision::Continue;
        if (text_box == &chunk_start_box)
            found_chunk_start = true;
        else if (found_chunk_start && svg_text_box_starts_text_chunk(*text_box))
            return TraversalDecision::Break;
        if (found_chunk_start && !text_box->dom_node().text_contents().is_empty()) {
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

static Gfx::Path compute_path_for_svg_text(SVGTextBox const& text_box, Gfx::FloatPoint current_text_position)
{
    auto& text_element = text_box.dom_node();
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

static Gfx::Path compute_path_for_svg_text_path(SVGTextPathBox const& text_path_box, CSSPixelSize viewport_size)
{
    auto& text_path_element = static_cast<SVG::SVGTextPathElement const&>(text_path_box.dom_node());
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
    auto const& graphics_box = as<SVGGraphicsBox>(node);
    CSSPixelSize viewport_size {
        CSSPixels::from_raw(request.viewport_width),
        CSSPixels::from_raw(request.viewport_height),
    };
    Gfx::FloatPoint text_position {
        request.current_text_position.x,
        request.current_text_position.y,
    };

    Gfx::Path path;
    if (auto const* geometry_box = as_if<SVGGeometryBox>(graphics_box)) {
        path = const_cast<SVGGeometryBox&>(*geometry_box).dom_node().get_path(viewport_size);
    } else if (auto const* text_box = as_if<SVGTextBox>(graphics_box)) {
        auto const& text_element = text_box->dom_node();
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
    } else if (auto const* text_path_box = as_if<SVGTextPathBox>(graphics_box)) {
        path = compute_path_for_svg_text_path(*text_path_box, viewport_size);
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

// Stamps the store once per pass entry, so cache entries validate against the
// viewport the pass actually laid out with; a new bridge entry method must call
// this before entering Rust.
static void note_viewport_size_for_pass(Box& pass_root)
{
    auto viewport_rect = pass_root.document().viewport_rect();
    RustFFI::layout_arena_note_viewport_size(
        pass_root.arena_handle(),
        viewport_rect.width().raw_value(),
        viewport_rect.height().raw_value());
}

void LayoutRustBridge::run_root_layout(Box& viewport, CSSPixels viewport_inline_size, CSSPixels viewport_block_size, bool should_collect_devtools_layout_data)
{
    VERIFY(!m_commit_root);
    m_commit_root = &viewport;
    ScopeGuard clear_commit_root = [&] {
        m_commit_root = nullptr;
    };

    viewport.document().invalidate_stacking_context_tree();
    note_viewport_size_for_pass(viewport);
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
    VERIFY(!m_line_commit_context);
}

void LayoutRustBridge::compute_subtree_layout(Box& root)
{
    VERIFY(!m_commit_root);
    m_commit_root = &root;
    ScopeGuard clear_commit_root = [&] {
        m_commit_root = nullptr;
    };

    root.document().invalidate_stacking_context_tree();
    note_viewport_size_for_pass(root);
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
    VERIFY(!m_line_commit_context);
}

void LayoutRustBridge::replay_saved_abspos_layout(Box& box)
{
    VERIFY(!m_commit_root);
    m_commit_root = &box;
    ScopeGuard clear_commit_root = [&] {
        m_commit_root = nullptr;
    };

    box.document().invalidate_stacking_context_tree();
    note_viewport_size_for_pass(box);
    auto callbacks = formatting_context_callbacks();
    auto sink = commit_sink();
    {
        ActiveLayoutPassScope active_pass;
        RustFFI::rust_layout_replay_saved_abspos_layout(Node::slot_id(&box), &callbacks, &sink);
    }
    VERIFY(!m_line_commit_context);
}

struct LayoutRustBridge::LineCommitContext {
    explicit LineCommitContext(Painting::PaintableWithLines& paintable)
        : paintable(paintable)
    {
    }

    Painting::PaintableWithLines& paintable;
    Vector<Painting::LineRecord> lines;
    Vector<Painting::InlineBoxPiece> pieces;
};

static CSS::BorderData from_ffi_border_data(RustFFI::FfiBorderData const&);

static Painting::Paintable::BorderDataWithElementKind from_ffi_border_data_with_element_kind(RustFFI::FfiBorderDataWithElementKind const& border)
{
    return {
        .border_data = from_ffi_border_data(border.border_data),
        .element_kind = static_cast<Painting::Paintable::ConflictingElementKind>(border.element_kind),
    };
}

RustFFI::FfiCommitSink LayoutRustBridge::commit_sink()
{
    return {
        .context = this,
        .begin_commit = [](void* context, void* root_pointer) {
            auto& bridge = *static_cast<LayoutRustBridge*>(context);
            auto& root = *static_cast<Box*>(root_pointer);
            VERIFY(!bridge.m_replaced_paintable);
            VERIFY(!bridge.m_commit_parent_paintable);
            VERIFY(!bridge.m_commit_insert_before_paintable);

            if (!root.is_viewport()) {
                bridge.m_replaced_paintable = root.paintable();
                if (!bridge.m_replaced_paintable && root.dom_node()) {
                    // A rebuilt box has no paintable yet; the previous one stays referenced by
                    // the DOM node (and alive in the paint tree) until this commit replaces it.
                    bridge.m_replaced_paintable = root.dom_node()->unsafe_paintable();
                }
            }

            if (bridge.m_replaced_paintable) {
                // Keep the old subtree alive while its replacement is spliced
                // into the exact same paint-order position.
                bridge.m_commit_parent_paintable = bridge.m_replaced_paintable->parent();
                bridge.m_commit_insert_before_paintable = bridge.m_replaced_paintable->next_sibling();
                if (bridge.m_commit_parent_paintable)
                    bridge.m_commit_parent_paintable->remove_child(*bridge.m_replaced_paintable);
            }

            return RustFFI::FfiCommitPosition {
                .parent_paintable = bridge.m_commit_parent_paintable.ptr(),
                .insert_before_paintable = bridge.m_commit_insert_before_paintable.ptr(),
            }; },
        .finish_commit = [](void* context) {
            auto& bridge = *static_cast<LayoutRustBridge*>(context);
            bridge.m_commit_insert_before_paintable = nullptr;
            bridge.m_commit_parent_paintable = nullptr;
            bridge.m_replaced_paintable = nullptr; },
        .prepare_node = [](void*, void* node_pointer, bool has_used_values) -> void* {
            auto& node = *static_cast<Node*>(node_pointer);

            RefPtr<Painting::Paintable> paintable;
            if (has_used_values || (node.is_fragmented_inline() && node.dom_node())) {
                // Inline boxes that never went through inline layout (so they have no used values) still
                // need a paintable so DOM geometry queries have something to answer from.
                paintable = node.paintable();
                if (paintable)
                    paintable->reset_for_relayout();
                else
                    paintable = node.create_paintable();
                node.set_paintable(paintable);
            } else if (node.paintable_ptr()) {
                // A paintable surviving from a previous layout on a node this pass did not lay out is
                // stale; drop it so the layout tree only points into the paint tree built by this commit.
                node.clear_paintable();
            }
            return paintable.ptr();
        },
        .set_box_metrics = [](void*, void* paintable_pointer, RustFFI::FfiCommittedBoxMetrics metrics) {
            auto& paintable = *static_cast<Painting::Paintable*>(paintable_pointer);
            paintable.set_layout_fragment_identity(metrics.fragment_identity);
            auto& box_model = paintable.box_model();
            box_model.inset = {
                CSSPixels::from_raw(metrics.inset_top),
                CSSPixels::from_raw(metrics.inset_right),
                CSSPixels::from_raw(metrics.inset_bottom),
                CSSPixels::from_raw(metrics.inset_left),
            };
            box_model.padding = {
                CSSPixels::from_raw(metrics.padding_top),
                CSSPixels::from_raw(metrics.padding_right),
                CSSPixels::from_raw(metrics.padding_bottom),
                CSSPixels::from_raw(metrics.padding_left),
            };
            box_model.border = {
                CSSPixels::from_raw(metrics.border_top),
                CSSPixels::from_raw(metrics.border_right),
                CSSPixels::from_raw(metrics.border_bottom),
                CSSPixels::from_raw(metrics.border_left),
            };
            box_model.margin = {
                CSSPixels::from_raw(metrics.margin_top),
                CSSPixels::from_raw(metrics.margin_right),
                CSSPixels::from_raw(metrics.margin_bottom),
                CSSPixels::from_raw(metrics.margin_left),
            };
            paintable.set_content_size(
                CSSPixels::from_raw(metrics.content_inline_size),
                CSSPixels::from_raw(metrics.content_block_size));
            paintable.set_offset({
                CSSPixels::from_raw(metrics.content_offset.x),
                CSSPixels::from_raw(metrics.content_offset.y),
            });
            if (metrics.has_containing_line_box_index)
                paintable.set_containing_line_box_index(metrics.containing_line_box_index); },
        .set_override_borders = [](void*, void* paintable_pointer, RustFFI::FfiBordersData borders) { static_cast<Painting::Paintable*>(paintable_pointer)->set_override_borders_data({
                                                                                                          .top = from_ffi_border_data_with_element_kind(borders.top),
                                                                                                          .right = from_ffi_border_data_with_element_kind(borders.right),
                                                                                                          .bottom = from_ffi_border_data_with_element_kind(borders.bottom),
                                                                                                          .left = from_ffi_border_data_with_element_kind(borders.left),
                                                                                                      }); },
        .set_table_cell_coordinates = [](void*, void* paintable_pointer, RustFFI::FfiTableCellCoordinates coordinates) { static_cast<Painting::Paintable*>(paintable_pointer)->set_table_cell_coordinates({
                                                                                                                             .row_index = coordinates.row_index,
                                                                                                                             .column_index = coordinates.column_index,
                                                                                                                             .row_span = coordinates.row_span,
                                                                                                                             .column_span = coordinates.column_span,
                                                                                                                         }); },
        .begin_line_data = [](void* context, void* paintable_pointer) {
            auto& bridge = *static_cast<LayoutRustBridge*>(context);
            VERIFY(!bridge.m_line_commit_context);
            auto* paintable = as_if<Painting::PaintableWithLines>(*static_cast<Painting::Paintable*>(paintable_pointer));
            if (!paintable)
                return false;
            bridge.m_line_commit_context = make<LineCommitContext>(*paintable);
            return true; },
        .begin_line = [](void* context, RustFFI::FfiLineRecord record) {
            auto& line_context = *static_cast<LayoutRustBridge*>(context)->m_line_commit_context;
            line_context.lines.append({
                .rect = {
                    CSSPixels::from_raw(record.rect.x),
                    CSSPixels::from_raw(record.rect.y),
                    CSSPixels::from_raw(record.rect.width),
                    CSSPixels::from_raw(record.rect.height),
                },
                .baseline = CSSPixels::from_raw(record.baseline),
                .fragment_count = record.committed_fragment_count,
            }); },
        .emit_fragment = [](void* context, RustFFI::FfiCommittedFragment fragment) {
            auto& line_context = *static_cast<LayoutRustBridge*>(context)->m_line_commit_context;
            VERIFY(fragment.layout_node);
            RefPtr<Gfx::GlyphRun> glyph_run;
            if (fragment.has_glyph_run) {
                VERIFY(fragment.glyph_font);
                VERIFY(fragment.glyphs || fragment.glyph_count == 0);
                Vector<Gfx::DrawGlyph> glyphs;
                glyphs.ensure_capacity(fragment.glyph_count);
                auto const* draw_glyphs = reinterpret_cast<Gfx::DrawGlyph const*>(fragment.glyphs);
                glyphs.unchecked_append(draw_glyphs, fragment.glyph_count);
                glyph_run = adopt_ref(*new Gfx::GlyphRun(
                    move(glyphs),
                    *static_cast<Gfx::Font const*>(fragment.glyph_font),
                    static_cast<Gfx::GlyphRun::TextType>(fragment.glyph_text_type),
                    fragment.glyph_run_width));
            }
            line_context.paintable.add_fragment({
                .layout_node = *static_cast<Node const*>(fragment.layout_node),
                .offset = {
                    CSSPixels::from_raw(fragment.offset.x),
                    CSSPixels::from_raw(fragment.offset.y),
                },
                .size = {
                    CSSPixels::from_raw(fragment.size.x),
                    CSSPixels::from_raw(fragment.size.y),
                },
                .line_index = static_cast<u32>(line_context.lines.size() - 1),
                .start_offset = fragment.start,
                .length_in_code_units = fragment.length_in_code_units,
                .glyph_run = move(glyph_run),
                .baseline = CSSPixels::from_raw(fragment.baseline),
                .accumulated_vertical_shift = CSSPixels::from_raw(fragment.accumulated_vertical_shift),
                .writing_mode = static_cast<CSS::WritingMode>(fragment.writing_mode),
                .has_trailing_whitespace = fragment.has_trailing_whitespace,
            }); },
        .emit_inline_box_piece = [](void* context, RustFFI::FfiInlineBoxPiece piece) {
            auto& line_context = *static_cast<LayoutRustBridge*>(context)->m_line_commit_context;
            VERIFY(piece.node);
            line_context.pieces.append({
                .node = *static_cast<Node const*>(piece.node),
                .first_fragment_index = piece.first_fragment_index,
                .fragment_count = piece.fragment_count,
                .line_index = piece.line_index,
                .border_box_rect = {
                    CSSPixels::from_raw(piece.border_box_rect.x),
                    CSSPixels::from_raw(piece.border_box_rect.y),
                    CSSPixels::from_raw(piece.border_box_rect.width),
                    CSSPixels::from_raw(piece.border_box_rect.height),
                },
                .baseline = CSSPixels::from_raw(piece.baseline),
                .accumulated_vertical_shift = CSSPixels::from_raw(piece.accumulated_vertical_shift),
                .present_edges = piece.present_edges,
                .is_geometry_only_placeholder = piece.is_geometry_only_placeholder,
            }); },
        .finish_line_data = [](void* context) {
            auto& bridge = *static_cast<LayoutRustBridge*>(context);
            auto line_context = move(bridge.m_line_commit_context);
            VERIFY(line_context);
            line_context->paintable.set_lines(move(line_context->lines));
            line_context->paintable.set_inline_box_pieces(move(line_context->pieces));

            // Piece fragment ranges were counted against the same skip-fully-truncated
            // fragment stream during inline layout; a divergence would let piece
            // consumers read out of bounds.
            for (auto const& piece : line_context->paintable.inline_box_pieces())
                VERIFY(piece.first_fragment_index + piece.fragment_count <= line_context->paintable.fragments().size()); },
        .set_svg_viewport_transform = [](void*, void* paintable_pointer, RustFFI::FfiAffineTransform transform) {
            auto& paintable = *static_cast<Painting::Paintable*>(paintable_pointer);
            paintable.set_svg_viewport_transform(from_ffi_affine_transform(transform)); },
        .set_svg_viewport_size = [](void*, void* paintable_pointer, RustFFI::FfiCssPixelSize viewport_size) {
            auto& paintable = *static_cast<Painting::Paintable*>(paintable_pointer);
            if (auto* svg_svg_paintable = as_if<Painting::SVGSVGPaintable>(paintable)) {
                svg_svg_paintable->set_svg_viewport_size({
                    CSSPixels::from_raw(viewport_size.width),
                    CSSPixels::from_raw(viewport_size.height),
                });
            } },
        .set_computed_svg_path = [](void*, void* paintable_pointer, void* path_pointer, u64 path_identity) {
            VERIFY(path_pointer);
            auto& paintable = *static_cast<Painting::Paintable*>(paintable_pointer);
            // The path stays owned by the Rust fragment tree, which may emit it again on a
            // later commit; the identity is process-unique per path allocation, so a match
            // means the preserved copy is already this exact path and the copy can be skipped.
            auto const* path = static_cast<Gfx::Path const*>(path_pointer);
            if (auto* svg_path_paintable = as_if<Painting::SVGPathPaintable>(paintable))
                svg_path_paintable->set_computed_path_if_identity_changed(*path, path_identity); },
        .set_grid_layout_data = [](void*, void* paintable_pointer, RustFFI::FfiGridLayoutData const* data) {
            VERIFY(data);
            static_cast<Painting::Paintable*>(paintable_pointer)->set_grid_layout_data(build_grid_layout_data(*data)); },
        .set_flex_layout_data = [](void*, void* paintable_pointer, RustFFI::FfiFlexLayoutData const* data) {
            VERIFY(data);
            static_cast<Painting::Paintable*>(paintable_pointer)->set_flex_layout_data(build_flex_layout_data(*data)); },
        .set_used_grid_tracks = [](void*, void* paintable_pointer, RustFFI::FfiUsedGridTrackList const* columns, RustFFI::FfiUsedGridTrackList const* rows) {
            VERIFY(columns);
            VERIFY(rows);
            auto& paintable = *static_cast<Painting::Paintable*>(paintable_pointer);
            paintable.set_used_values_for_grid_template_columns(CSS::GridTrackSizeListStyleValue::create(build_used_grid_track_list(*columns)));
            paintable.set_used_values_for_grid_template_rows(CSS::GridTrackSizeListStyleValue::create(build_used_grid_track_list(*rows))); },
        .finish_node = [](void*, void* node_pointer, void* paintable_pointer, void* parent_paintable_pointer, void* insert_before_paintable_pointer) {
            auto& node = *static_cast<Node*>(node_pointer);
            auto* paintable = static_cast<Painting::Paintable*>(paintable_pointer);
            auto* parent_paintable = static_cast<Painting::Paintable*>(parent_paintable_pointer);
            auto* insert_before_paintable = static_cast<Painting::Paintable*>(insert_before_paintable_pointer);
            auto* dom_node = node.dom_node();
            Painting::Paintable* paintable_for_children = nullptr;
            if (paintable) {
                if (parent_paintable && !paintable->forms_unconnected_subtree()) {
                    VERIFY(!paintable->parent());
                    parent_paintable->insert_before(*paintable, insert_before_paintable);
                }
                paintable->set_dom_node(dom_node);
                if (dom_node)
                    dom_node->set_paintable(paintable);
                auto* containing_block = node.containing_block();
                paintable->set_containing_block(containing_block ? containing_block->paintable_ptr() : nullptr);
                paintable_for_children = paintable;
            } else {
                if (dom_node)
                    dom_node->clear_paintable();
                // An inline box without a paintable must not orphan its descendants' paintables; pass the
                // nearest ancestor paintable through. Other paintable-less nodes (e.g. non-rendered SVG
                // subtrees) keep their descendants disconnected on purpose.
                if (node.is_fragmented_inline())
                    paintable_for_children = parent_paintable;
            }
            return RustFFI::FfiCommitNodeResult {
                .paintable = paintable,
                .paintable_for_children = paintable_for_children,
            }; },
        .assign_inline_box_geometry = [](void*, void* paintable_pointer) { as<Painting::PaintableWithLines>(*static_cast<Painting::Paintable*>(paintable_pointer)).assign_inline_box_geometry(); },
    };
}

static CSS::BorderData from_ffi_border_data(RustFFI::FfiBorderData const& border)
{
    return {
        .color = Gfx::Color::from_bgra(border.color),
        .line_style = static_cast<CSS::LineStyle>(border.line_style),
        .width = CSSPixels::from_raw(border.width),
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
    static_assert(to_underlying(Painting::Paintable::ConflictingElementKind::Cell) == 0);
    static_assert(to_underlying(Painting::Paintable::ConflictingElementKind::Row) == 1);
    static_assert(to_underlying(Painting::Paintable::ConflictingElementKind::RowGroup) == 2);
    static_assert(to_underlying(Painting::Paintable::ConflictingElementKind::Column) == 3);
    static_assert(to_underlying(Painting::Paintable::ConflictingElementKind::ColumnGroup) == 4);
    static_assert(to_underlying(Painting::Paintable::ConflictingElementKind::Table) == 5);
    static_assert(to_underlying(FlexLayoutGrowthState::Growing) == 0);
    static_assert(to_underlying(FlexLayoutGrowthState::Shrinking) == 1);
    static_assert(to_underlying(FlexLayoutClampState::Unclamped) == 0);
    static_assert(to_underlying(FlexLayoutClampState::ClampedToMin) == 1);
    static_assert(to_underlying(FlexLayoutClampState::ClampedToMax) == 2);
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
    static_assert(to_underlying(Gfx::GlyphRun::TextType::Common) == 0);
    static_assert(to_underlying(Gfx::GlyphRun::TextType::ContextDependent) == 1);
    static_assert(to_underlying(Gfx::GlyphRun::TextType::EndPadding) == 2);
    static_assert(to_underlying(Gfx::GlyphRun::TextType::Ltr) == 3);
    static_assert(to_underlying(Gfx::GlyphRun::TextType::Rtl) == 4);
    static_assert(sizeof(RustFFI::FfiDrawGlyph) == sizeof(Gfx::DrawGlyph));
    static_assert(alignof(RustFFI::FfiDrawGlyph) == alignof(Gfx::DrawGlyph));
    static_assert(IsTriviallyCopyable<RustFFI::FfiDrawGlyph>);
    static_assert(IsTriviallyCopyable<Gfx::DrawGlyph>);
    static_assert(sizeof(Gfx::FloatPoint) == 2 * sizeof(float));
    static_assert(offsetof(RustFFI::FfiDrawGlyph, x) == offsetof(Gfx::DrawGlyph, position));
    static_assert(offsetof(RustFFI::FfiDrawGlyph, y) == offsetof(Gfx::DrawGlyph, position) + sizeof(float));
    static_assert(offsetof(RustFFI::FfiDrawGlyph, length_in_code_units) == offsetof(Gfx::DrawGlyph, length_in_code_units));
    static_assert(offsetof(RustFFI::FfiDrawGlyph, glyph_width) == offsetof(Gfx::DrawGlyph, glyph_width));
    static_assert(offsetof(RustFFI::FfiDrawGlyph, glyph_id) == offsetof(Gfx::DrawGlyph, glyph_id));
    static_assert(offsetof(RustFFI::FfiDrawGlyph, should_paint) == offsetof(Gfx::DrawGlyph, should_paint));

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
            auto const& image_box = as<SVGImageBox>(*static_cast<Node const*>(node));
            auto bounding_box = image_box.dom_node().bounding_box({
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
