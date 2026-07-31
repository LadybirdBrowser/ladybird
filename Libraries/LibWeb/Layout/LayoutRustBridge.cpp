/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
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
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/DominantBaseline.h>
#include <LibWeb/Layout/FieldSetBox.h>
#include <LibWeb/Layout/FlexLayoutData.h>
#include <LibWeb/Layout/GridLayoutData.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/ListItemBox.h>
#include <LibWeb/Layout/ListItemMarkerBox.h>
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
#include <LibWeb/Layout/TextInputBox.h>
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

namespace Web::Layout {

static Atomic<size_t> s_outstanding_anchor_name_handles;
static Atomic<size_t> s_outstanding_svg_path_handles;

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
            auto const& values = item_box.computed_values();
            auto const& flex_basis = values.flex_basis();
            auto const& main_size = main_axis_is_horizontal ? values.width() : values.height();
            auto const& main_min_size = main_axis_is_horizontal ? values.min_width() : values.min_height();
            auto const& main_max_size = main_axis_is_horizontal ? values.max_width() : values.max_height();

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
        element_transform = graphics_element->element_transform();
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

    bool has_own_view_box = false;
    if (auto const* svg_element = as_if<SVG::SVGSVGElement>(*dom_node))
        has_own_view_box = svg_element->view_box().has_value();

    return {
        .is_document_element = node.document().document_element() == dom_node,
        .document_is_decoded_svg = node.document().is_decoded_svg(),
        .is_fit_to_view_box = is<SVG::SVGFitToViewBox>(*dom_node),
        .has_active_view_box = active_view_box.has_value(),
        .active_view_box = active_view_box.has_value() ? to_ffi_svg_view_box(*active_view_box) : RustFFI::FfiSvgViewBox {},
        .has_own_view_box = has_own_view_box,
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

static Gfx::Path compute_path_for_svg_text(SVGTextBox const& text_box, Gfx::FloatPoint current_text_position)
{
    auto& text_element = text_box.dom_node();
    // FIXME: Use per-code-point fonts.
    auto& font = text_box.first_available_font();
    auto text_contents = text_element.text_contents();
    auto text_width = font.width(text_contents);
    auto text_offset = current_text_position;

    switch (text_element.text_anchor().value_or(SVG::TextAnchor::Start)) {
    case SVG::TextAnchor::Start:
        break;
    case SVG::TextAnchor::Middle:
        text_offset.translate_by(-text_width / 2, 0);
        break;
    case SVG::TextAnchor::End:
        text_offset.translate_by(-text_width, 0);
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    auto baseline_metric = resolve_dominant_baseline_metric(text_box.computed_values());
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
        auto text_positioning = text_box->dom_node().text_positioning();
        text_positioning.apply_to_text_position(viewport_size, text_position, 0u);
        path = compute_path_for_svg_text(*text_box, text_position);
    } else if (auto const* text_path_box = as_if<SVGTextPathBox>(graphics_box)) {
        path = compute_path_for_svg_text_path(*text_path_box, viewport_size);
    }

    auto bounding_box = path.bounding_box();
    auto* path_handle = new Gfx::Path(move(path));
    ++s_outstanding_svg_path_handles;
    return {
        .path_handle = path_handle,
        .bounding_box = {
            .x = bounding_box.x(),
            .y = bounding_box.y(),
            .width = bounding_box.width(),
            .height = bounding_box.height(),
        },
        .text_position_for_children = {
            .x = text_position.x(),
            .y = text_position.y(),
        },
    };
}

static void release_svg_path_handle(void const* handle)
{
    VERIFY(handle);
    VERIFY(s_outstanding_svg_path_handles.load() > 0);
    --s_outstanding_svg_path_handles;
    delete static_cast<Gfx::Path const*>(handle);
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
    viewport.document().layout_node_arena().sync_enrolled_text_node_content();
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

void LayoutRustBridge::compute_subtree_layout(Box& root, Painting::Paintable& paintable_to_replace)
{
    VERIFY(!m_commit_root);
    m_commit_root = &root;
    ScopeGuard clear_commit_root = [&] {
        m_commit_root = nullptr;
    };

    root.document().invalidate_stacking_context_tree();
    root.document().layout_node_arena().sync_enrolled_text_node_content();
    auto viewport_rect = root.document().viewport_rect();
    auto callbacks = formatting_context_callbacks();
    auto sink = commit_sink();
    {
        ActiveLayoutPassScope active_pass;
        RustFFI::rust_layout_compute_subtree_layout(
            Node::slot_id(&root),
            Node::slot_id(&root.root()),
            &paintable_to_replace,
            viewport_rect.width().raw_value(),
            viewport_rect.height().raw_value(),
            &callbacks,
            &sink);
    }
    VERIFY(!m_line_commit_context);
}

void LayoutRustBridge::replay_saved_abspos_layout(Box& box, Painting::Paintable& paintable_to_replace)
{
    VERIFY(!m_commit_root);
    m_commit_root = &box;
    ScopeGuard clear_commit_root = [&] {
        m_commit_root = nullptr;
    };

    box.document().invalidate_stacking_context_tree();
    box.document().layout_node_arena().sync_enrolled_text_node_content();
    auto callbacks = formatting_context_callbacks();
    auto sink = commit_sink();
    {
        ActiveLayoutPassScope active_pass;
        RustFFI::rust_layout_replay_saved_abspos_layout(Node::slot_id(&box), &paintable_to_replace, &callbacks, &sink);
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
        .begin_commit = [](void* context, void* root_pointer, void* paintable_to_replace_pointer) {
            auto& bridge = *static_cast<LayoutRustBridge*>(context);
            auto& root = *static_cast<Box*>(root_pointer);
            VERIFY(!bridge.m_replaced_paintable);
            VERIFY(!bridge.m_commit_parent_paintable);
            VERIFY(!bridge.m_commit_insert_before_paintable);

            if (paintable_to_replace_pointer) {
                bridge.m_replaced_paintable = *static_cast<Painting::Paintable*>(paintable_to_replace_pointer);
            } else if (!root.is_viewport()) {
                bridge.m_replaced_paintable = root.paintable();
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
                .border_box_rect = {
                    CSSPixels::from_raw(piece.border_box_rect.x),
                    CSSPixels::from_raw(piece.border_box_rect.y),
                    CSSPixels::from_raw(piece.border_box_rect.width),
                    CSSPixels::from_raw(piece.border_box_rect.height),
                },
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
        .set_computed_svg_transforms = [](void*, void* paintable_pointer, RustFFI::FfiSvgComputedTransforms transforms) {
            Painting::SVGGraphicsPaintable::ComputedTransforms converted {
                from_ffi_affine_transform(transforms.viewbox_transform),
                from_ffi_affine_transform(transforms.svg_transform),
            };
            auto& paintable = *static_cast<Painting::Paintable*>(paintable_pointer);
            if (auto* svg_graphics_paintable = as_if<Painting::SVGGraphicsPaintable>(paintable))
                svg_graphics_paintable->set_computed_transforms(converted);
            if (auto* svg_foreign_object_paintable = as_if<Painting::SVGForeignObjectPaintable>(paintable))
                svg_foreign_object_paintable->set_computed_transforms(converted);
            if (auto* svg_svg_paintable = as_if<Painting::SVGSVGPaintable>(paintable))
                svg_svg_paintable->set_computed_transforms(converted); },
        .set_svg_viewport_size = [](void*, void* paintable_pointer, RustFFI::FfiCssPixelSize viewport_size) {
            auto& paintable = *static_cast<Painting::Paintable*>(paintable_pointer);
            if (auto* svg_svg_paintable = as_if<Painting::SVGSVGPaintable>(paintable)) {
                svg_svg_paintable->set_svg_viewport_size({
                    CSSPixels::from_raw(viewport_size.width),
                    CSSPixels::from_raw(viewport_size.height),
                });
            } },
        .set_computed_svg_path = [](void*, void* paintable_pointer, void* path_pointer) {
            VERIFY(path_pointer);
            auto& paintable = *static_cast<Painting::Paintable*>(paintable_pointer);
            auto* path = static_cast<Gfx::Path*>(path_pointer);
            if (auto* svg_path_paintable = as_if<Painting::SVGPathPaintable>(paintable))
                svg_path_paintable->set_computed_path(move(*path));
            release_svg_path_handle(path); },
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

static bool style_value_contains_anchor(CSS::StyleValue const& value)
{
    if (value.is_anchor())
        return true;
    if (value.is_calculated())
        return value.as_calculated().contains_anchor_function();
    return false;
}

bool box_inset_properties_contain_anchor_functions(Box const& box)
{
    auto abstract_element = abstract_element_for_abspos_box(box);
    if (!abstract_element.has_value())
        return false;

    auto const* computed = abstract_element->computed_values();
    if (!computed)
        return false;
    // Anchor functions in insets only survive to used-value time inside calculated values, so
    // when no inset is calculated (the common case), skip reconstructing the style values.
    auto const& inset = computed->inset();
    if (!inset.top().is_calculated() && !inset.right().is_calculated() && !inset.bottom().is_calculated() && !inset.left().is_calculated())
        return false;

    auto top = computed->computed_style_value(CSS::PropertyID::Top);
    auto right = computed->computed_style_value(CSS::PropertyID::Right);
    auto bottom = computed->computed_style_value(CSS::PropertyID::Bottom);
    auto left = computed->computed_style_value(CSS::PropertyID::Left);
    VERIFY(top && right && bottom && left);
    return style_value_contains_anchor(*top)
        || style_value_contains_anchor(*right)
        || style_value_contains_anchor(*bottom)
        || style_value_contains_anchor(*left);
}

bool can_replay_saved_abspos_layout_inputs_after_style_change(Box const& box)
{
    if (!box.containing_block())
        return false;

    if (box.saved_abspos_cb_derives_from_own_computed_values())
        return false;

    auto const& inset = box.computed_values().inset();
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
    static_assert(to_underlying(RustFFI::FfiAnchorSideKind::Invalid) == 0);
    static_assert(to_underlying(RustFFI::FfiAnchorSideKind::Top) == 1);
    static_assert(to_underlying(RustFFI::FfiAnchorSideKind::Right) == 2);
    static_assert(to_underlying(RustFFI::FfiAnchorSideKind::Bottom) == 3);
    static_assert(to_underlying(RustFFI::FfiAnchorSideKind::Left) == 4);
    static_assert(to_underlying(RustFFI::FfiAnchorSideKind::Center) == 5);
    static_assert(to_underlying(RustFFI::FfiAnchorSideKind::Start) == 6);
    static_assert(to_underlying(RustFFI::FfiAnchorSideKind::End) == 7);
    static_assert(to_underlying(RustFFI::FfiAnchorSideKind::SelfStart) == 8);
    static_assert(to_underlying(RustFFI::FfiAnchorSideKind::SelfEnd) == 9);
    static_assert(to_underlying(RustFFI::FfiAnchorSideKind::Inside) == 10);
    static_assert(to_underlying(RustFFI::FfiAnchorSideKind::Outside) == 11);
    static_assert(to_underlying(RustFFI::FfiAnchorSideKind::Percentage) == 12);
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
        .static_position_containing_block = [](void*, void* node) { return Node::slot_id(static_cast<Box const*>(node)->static_position_containing_block()); },
        .needs_inset_resolution = [](void*, void* node) {
            auto const& styled_node = *static_cast<NodeWithStyleAndBoxModelMetrics const*>(node);
            if (styled_node.computed_values().position() == CSS::Positioning::Relative)
                return true;
            auto const* box = as_if<Box>(styled_node);
            return box && box_inset_properties_contain_anchor_functions(*box); },
        .report_unexpected_fragmented_inline = [](void*, void* node) {
            auto const& box = *static_cast<Box const*>(node);
            dbgln("FIXME: InlineFormattingContext::dimension_box_on_line got unexpected box in inline context:");
            dump_tree(box); },
        .release_anchor_name_handle = ladybird_layout_release_anchor_name_handle,
        .build_replaced_content_facts = [](void*, void* node) {
            RustFFI::FfiReplacedContentFacts facts {};
            auto const* box = as_if<Box>(*static_cast<Node const*>(node));
            if (!box)
                return facts;
            auto auto_content_size = box->auto_content_box_size();
            facts.has_auto_content_width = auto_content_size.has_width();
            facts.auto_content_width = auto_content_size.width.value_or(0).raw_value();
            facts.has_auto_content_height = auto_content_size.has_height();
            facts.auto_content_height = auto_content_size.height.value_or(0).raw_value();
            if (auto_content_size.aspect_ratio.has_value()) {
                facts.auto_content_aspect_ratio_numerator = auto_content_size.aspect_ratio->numerator().raw_value();
                facts.auto_content_aspect_ratio_denominator = auto_content_size.aspect_ratio->denominator().raw_value();
            }
            if (box->computed_values().appearance() == CSS::Appearance::None) {
                if (auto const* input = as_if<HTML::HTMLInputElement>(box->dom_node())) {
                    switch (input->type_state()) {
                    case HTML::HTMLInputElement::TypeAttributeState::Text:
                    case HTML::HTMLInputElement::TypeAttributeState::Search:
                    case HTML::HTMLInputElement::TypeAttributeState::URL:
                    case HTML::HTMLInputElement::TypeAttributeState::Telephone:
                    case HTML::HTMLInputElement::TypeAttributeState::Email:
                    case HTML::HTMLInputElement::TypeAttributeState::Password:
                    case HTML::HTMLInputElement::TypeAttributeState::Number: {
                        auto default_preferred_size = TextInputBox::default_preferred_size_for_text_control(*input, *box);
                        facts.has_default_preferred_width = default_preferred_size.has_width();
                        facts.default_preferred_width = default_preferred_size.width.value_or(0).raw_value();
                        facts.has_default_preferred_height = default_preferred_size.has_height();
                        facts.default_preferred_height = default_preferred_size.height.value_or(0).raw_value();
                        break;
                    }
                    default:
                        break;
                    }
                }
            }
            return facts; },
        .build_list_item_facts = [](void*, void* node) {
            RustFFI::FfiListItemFacts facts {};
            auto const& layout_node = *static_cast<Node const*>(node);
            if (auto const* list_item_box = as_if<ListItemBox>(layout_node))
                facts.marker = Node::slot_id(list_item_box->marker());
            if (auto const* marker = as_if<ListItemMarkerBox>(layout_node)) {
                facts.marker_is_symbolic = marker->is_symbolic();
                if (facts.marker_is_symbolic) {
                    CSSPixels marker_content_inline_size = 0;
                    CSSPixels marker_content_block_size = 0;
                    if (auto const* list_style_image = marker->list_style_image()) {
                        marker_content_inline_size = list_style_image->natural_width(marker->document()).value_or(0);
                        marker_content_block_size = list_style_image->natural_height(marker->document()).value_or(0);
                    } else {
                        auto marker_size = marker->relative_size();
                        marker_content_inline_size = marker_size;
                        marker_content_block_size = marker_size;
                    }
                    facts.marker_content_inline_size = marker_content_inline_size.raw_value();
                    facts.marker_content_block_size = marker_content_block_size.raw_value();
                    if (marker->has_symbolic_counter_style())
                        facts.marker_distance = CSSPixels::nearest_value_for(.5f * marker->first_available_font().pixel_size()).raw_value();
                }
                facts.marker_list_style_position = static_cast<u8>(to_underlying(marker->list_style_position()));
            }
            return facts; },
        .build_svg_facts = [](void*, void* node) {
            auto const* node_with_style = as_if<NodeWithStyle>(*static_cast<Node const*>(node));
            VERIFY(node_with_style);
            return build_svg_element_facts(*node_with_style); },
        .read_paintable_geometry = [](void*, void* node, void* paintable_pointer, RustFFI::FfiPaintableGeometry* out) {
            VERIFY(out);
            VERIFY(paintable_pointer);
            auto const* paintable = static_cast<Painting::Paintable const*>(paintable_pointer);
            auto const& box_model = paintable->box_model();
            *out = {
                .content_inline_size = paintable->content_width().raw_value(),
                .content_block_size = paintable->content_height().raw_value(),
                .content_offset = {
                    .x = paintable->offset().x().raw_value(),
                    .y = paintable->offset().y().raw_value(),
                },
                .svg_viewport_size = {},
                .margin_left = box_model.margin.left.raw_value(),
                .margin_right = box_model.margin.right.raw_value(),
                .margin_top = box_model.margin.top.raw_value(),
                .margin_bottom = box_model.margin.bottom.raw_value(),
                .border_left = box_model.border.left.raw_value(),
                .border_right = box_model.border.right.raw_value(),
                .border_top = box_model.border.top.raw_value(),
                .border_bottom = box_model.border.bottom.raw_value(),
                .padding_left = box_model.padding.left.raw_value(),
                .padding_right = box_model.padding.right.raw_value(),
                .padding_top = box_model.padding.top.raw_value(),
                .padding_bottom = box_model.padding.bottom.raw_value(),
                .inset_left = box_model.inset.left.raw_value(),
                .inset_right = box_model.inset.right.raw_value(),
                .inset_top = box_model.inset.top.raw_value(),
                .inset_bottom = box_model.inset.bottom.raw_value(),
            };

            // NB: We check the node type rather than the paintable type to mirror the rust-side logic.
            if (is<SVGSVGBox>(*static_cast<Node const*>(node))) {
                auto const* svg_svg_paintable = as_if<Painting::SVGSVGPaintable>(paintable);
                VERIFY(svg_svg_paintable);
                out->svg_viewport_size = {
                    .width = svg_svg_paintable->svg_viewport_size().width().raw_value(),
                    .height = svg_svg_paintable->svg_viewport_size().height().raw_value(),
                };
            }
            return true; },
        .read_paintable_svg_transforms = [](void*, void* node, RustFFI::FfiSvgComputedTransforms* out) {
            VERIFY(out);
            auto const* paintable = static_cast<Node const*>(node)->paintable_ptr();
            Painting::SVGGraphicsPaintable::ComputedTransforms const* transforms = nullptr;
            if (auto const* svg_graphics_paintable = as_if<Painting::SVGGraphicsPaintable>(paintable))
                transforms = &svg_graphics_paintable->computed_transforms();
            if (auto const* svg_foreign_object_paintable = as_if<Painting::SVGForeignObjectPaintable>(paintable))
                transforms = &svg_foreign_object_paintable->computed_transforms();
            if (auto const* svg_svg_paintable = as_if<Painting::SVGSVGPaintable>(paintable))
                transforms = &svg_svg_paintable->computed_transforms();
            if (!transforms)
                return false;
            *out = {
                .viewbox_transform = to_ffi_affine_transform(transforms->svg_to_viewbox_transform()),
                .svg_transform = to_ffi_affine_transform(transforms->svg_transform()),
            };
            return true; },
        .compute_svg_path = [](void*, void* node, RustFFI::FfiSvgPathRequest request) {
            auto const* node_with_style = as_if<NodeWithStyle>(*static_cast<Node const*>(node));
            VERIFY(node_with_style);
            return compute_svg_path(*node_with_style, request); },
        .release_svg_path = [](void*, void* path_handle) { release_svg_path_handle(path_handle); },
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
        .build_anchor_function_facts = [](void*, void const* shell) {
            auto value = CSS::StyleValue::adopt_rust_style_value_data(
                CSS::StyleValueFFI::rust_style_value_retain(
                    static_cast<CSS::StyleValueFFI::StyleValueData const*>(shell)));
            if (!value->is_anchor()) {
                return RustFFI::FfiAnchorFunctionFacts {
                    .has_anchor_name = false,
                    .anchor_name = 0,
                    .side_kind = RustFFI::FfiAnchorSideKind::Invalid,
                    .side_percentage = 0,
                };
            }
            auto const& anchor = value->as_anchor();
            auto const& side = *anchor.anchor_side();
            auto side_kind = RustFFI::FfiAnchorSideKind::Invalid;
            double side_percentage = 0;
            if (side.is_keyword()) {
                switch (side.to_keyword()) {
                case CSS::Keyword::Top:
                    side_kind = RustFFI::FfiAnchorSideKind::Top;
                    break;
                case CSS::Keyword::Right:
                    side_kind = RustFFI::FfiAnchorSideKind::Right;
                    break;
                case CSS::Keyword::Bottom:
                    side_kind = RustFFI::FfiAnchorSideKind::Bottom;
                    break;
                case CSS::Keyword::Left:
                    side_kind = RustFFI::FfiAnchorSideKind::Left;
                    break;
                case CSS::Keyword::Center:
                    side_kind = RustFFI::FfiAnchorSideKind::Center;
                    break;
                case CSS::Keyword::Start:
                    side_kind = RustFFI::FfiAnchorSideKind::Start;
                    break;
                case CSS::Keyword::End:
                    side_kind = RustFFI::FfiAnchorSideKind::End;
                    break;
                case CSS::Keyword::SelfStart:
                    side_kind = RustFFI::FfiAnchorSideKind::SelfStart;
                    break;
                case CSS::Keyword::SelfEnd:
                    side_kind = RustFFI::FfiAnchorSideKind::SelfEnd;
                    break;
                case CSS::Keyword::Inside:
                    side_kind = RustFFI::FfiAnchorSideKind::Inside;
                    break;
                case CSS::Keyword::Outside:
                    side_kind = RustFFI::FfiAnchorSideKind::Outside;
                    break;
                default:
                    break;
                }
            } else if (side.is_percentage()) {
                side_kind = RustFFI::FfiAnchorSideKind::Percentage;
                side_percentage = side.as_percentage().percentage().as_fraction();
            }
            size_t anchor_name = 0;
            if (auto name = anchor.anchor_name(); name.has_value()) {
                anchor_name = name->to_raw_leaked();
                ++s_outstanding_anchor_name_handles;
            }
            return RustFFI::FfiAnchorFunctionFacts {
                .has_anchor_name = anchor_name != 0,
                .anchor_name = anchor_name,
                .side_kind = side_kind,
                .side_percentage = side_percentage,
            }; },
        .anchor_function_fallback = [](void*, void const* shell) {
            auto value = CSS::StyleValue::adopt_rust_style_value_data(
                CSS::StyleValueFFI::rust_style_value_retain(
                    static_cast<CSS::StyleValueFFI::StyleValueData const*>(shell)));
            if (!value->is_anchor())
                return RustFFI::FfiAnchorFallbackFacts {
                    .kind = RustFFI::FfiAnchorFallbackKind::None,
                    .px = 0,
                    .fraction = 0,
                    .value = nullptr,
                };
            auto fallback = value->as_anchor().fallback_value();
            if (!fallback)
                return RustFFI::FfiAnchorFallbackFacts {
                    .kind = RustFFI::FfiAnchorFallbackKind::None,
                    .px = 0,
                    .fraction = 0,
                    .value = nullptr,
                };
            if (fallback->is_length()) {
                VERIFY(fallback->as_length().length().is_absolute());
                return RustFFI::FfiAnchorFallbackFacts {
                    .kind = RustFFI::FfiAnchorFallbackKind::Px,
                    .px = fallback->as_length().length().absolute_length_to_px().raw_value(),
                    .fraction = 0,
                    .value = nullptr,
                };
            }
            if (fallback->is_percentage()) {
                return RustFFI::FfiAnchorFallbackFacts {
                    .kind = RustFFI::FfiAnchorFallbackKind::Percentage,
                    .px = 0,
                    .fraction = fallback->as_percentage().percentage().as_fraction(),
                    .value = nullptr,
                };
            }
            if (fallback->is_calculated()) {
                return RustFFI::FfiAnchorFallbackFacts {
                    .kind = RustFFI::FfiAnchorFallbackKind::Calculated,
                    .px = 0,
                    .fraction = 0,
                    .value = fallback->as_calculated().rust_style_value_data(),
                };
            }
            VERIFY(fallback->is_anchor());
            return RustFFI::FfiAnchorFallbackFacts {
                .kind = RustFFI::FfiAnchorFallbackKind::Anchor,
                .px = 0,
                .fraction = 0,
                .value = fallback->rust_style_value_data(),
            }; },
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

extern "C" WEB_API void ladybird_layout_release_anchor_name_handle(size_t raw)
{
    VERIFY(Web::Layout::s_outstanding_anchor_name_handles.load() > 0);
    --Web::Layout::s_outstanding_anchor_name_handles;
    Utf16FlyString::unref_raw(raw);
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
