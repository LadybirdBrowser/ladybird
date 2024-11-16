/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/ScalingMode.h>
#include <LibWeb/CSS/CalculatedOr.h>
#include <LibWeb/CSS/Clip.h>
#include <LibWeb/CSS/ColumnCount.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/CSS/Display.h>
#include <LibWeb/CSS/Filter.h>
#include <LibWeb/CSS/GridTrackPlacement.h>
#include <LibWeb/CSS/GridTrackSize.h>
#include <LibWeb/CSS/LengthBox.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/Ratio.h>
#include <LibWeb/CSS/Size.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/BasicShapeStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/Transformation.h>

namespace Web::CSS {

using ClipRule = FillRule;

struct FlexBasisContent { };
using FlexBasis = Variant<FlexBasisContent, Size>;

struct AspectRatio {
    bool use_natural_aspect_ratio_if_available;
    Optional<Ratio> preferred_ratio;
};

struct GridAutoFlow {
    bool row { true };
    bool dense { false };
};

struct NormalGap { };

struct QuotesData {
    enum class Type {
        None,
        Auto,
        Specified,
    } type;
    Vector<Array<FlyString, 2>> strings {};
};

struct ResolvedFilter {
    struct Blur {
        float radius;
    };

    struct DropShadow {
        double offset_x;
        double offset_y;
        double radius;
        Gfx::Color color;
    };

    struct HueRotate {
        float angle_degrees;
    };

    struct Color {
        FilterOperation::Color::Type type;
        float amount;
    };

    using FilterFunction = Variant<Blur, DropShadow, HueRotate, Color>;

    bool is_none() const { return filters.size() == 0; }

    Vector<FilterFunction> filters;
};

struct ObjectPosition {
    PositionEdge edge_x { PositionEdge::Left };
    CSS::LengthPercentage offset_x { Percentage(50) };
    PositionEdge edge_y { PositionEdge::Top };
    CSS::LengthPercentage offset_y { Percentage(50) };
};

class InitialValues {
public:
    static AspectRatio aspect_ratio() { return AspectRatio { true, {} }; }
    static CSSPixels font_size() { return 16; }
    static int font_weight() { return 400; }
    static CSS::FontVariant font_variant() { return CSS::FontVariant::Normal; }
    static CSSPixels line_height() { return 0; }
    static CSS::Float float_() { return CSS::Float::None; }
    static CSS::Length border_spacing() { return CSS::Length::make_px(0); }
    static CSS::CaptionSide caption_side() { return CSS::CaptionSide::Top; }
    static CSS::Clear clear() { return CSS::Clear::None; }
    static CSS::Clip clip() { return CSS::Clip::make_auto(); }
    static CSS::ContentVisibility content_visibility() { return CSS::ContentVisibility::Visible; }
    static CSS::Cursor cursor() { return CSS::Cursor::Auto; }
    static CSS::WhiteSpace white_space() { return CSS::WhiteSpace::Normal; }
    static CSS::WordBreak word_break() { return CSS::WordBreak::Normal; }
    static CSS::LengthOrCalculated word_spacing() { return CSS::Length::make_px(0); }
    static LengthOrCalculated letter_spacing() { return CSS::Length::make_px(0); }
    static Variant<LengthOrCalculated, NumberOrCalculated> tab_size() { return NumberOrCalculated(8.0f); }
    static CSS::TextAlign text_align() { return CSS::TextAlign::Start; }
    static CSS::TextJustify text_justify() { return CSS::TextJustify::Auto; }
    static CSS::Positioning position() { return CSS::Positioning::Static; }
    static CSS::TextDecorationLine text_decoration_line() { return CSS::TextDecorationLine::None; }
    static CSS::Length text_decoration_thickness() { return Length::make_auto(); }
    static CSS::TextDecorationStyle text_decoration_style() { return CSS::TextDecorationStyle::Solid; }
    static CSS::TextTransform text_transform() { return CSS::TextTransform::None; }
    static CSS::TextOverflow text_overflow() { return CSS::TextOverflow::Clip; }
    static CSS::LengthPercentage text_indent() { return CSS::Length::make_px(0); }
    static CSS::Display display() { return CSS::Display { CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow }; }
    static Color color() { return Color::Black; }
    static Color stop_color() { return Color::Black; }
    static CSS::ResolvedFilter backdrop_filter() { return ResolvedFilter { .filters = {} }; }
    static CSS::ResolvedFilter filter() { return ResolvedFilter { .filters = {} }; }
    static Color background_color() { return Color::Transparent; }
    static CSS::ListStyleType list_style_type() { return CSS::ListStyleType::Disc; }
    static CSS::ListStylePosition list_style_position() { return CSS::ListStylePosition::Outside; }
    static CSS::Visibility visibility() { return CSS::Visibility::Visible; }
    static CSS::FlexDirection flex_direction() { return CSS::FlexDirection::Row; }
    static CSS::FlexWrap flex_wrap() { return CSS::FlexWrap::Nowrap; }
    static CSS::FlexBasis flex_basis() { return CSS::Size::make_auto(); }
    static CSS::ImageRendering image_rendering() { return CSS::ImageRendering::Auto; }
    static CSS::JustifyContent justify_content() { return CSS::JustifyContent::FlexStart; }
    static CSS::JustifyItems justify_items() { return CSS::JustifyItems::Legacy; }
    static CSS::JustifySelf justify_self() { return CSS::JustifySelf::Auto; }
    static CSS::AlignContent align_content() { return CSS::AlignContent::Stretch; }
    static CSS::AlignItems align_items() { return CSS::AlignItems::Stretch; }
    static CSS::AlignSelf align_self() { return CSS::AlignSelf::Auto; }
    static CSS::Appearance appearance() { return CSS::Appearance::Auto; }
    static CSS::Overflow overflow() { return CSS::Overflow::Visible; }
    static CSS::BoxSizing box_sizing() { return CSS::BoxSizing::ContentBox; }
    static CSS::PointerEvents pointer_events() { return CSS::PointerEvents::Auto; }
    static float flex_grow() { return 0.0f; }
    static float flex_shrink() { return 1.0f; }
    static int order() { return 0; }
    static float opacity() { return 1.0f; }
    static float fill_opacity() { return 1.0f; }
    static CSS::FillRule fill_rule() { return CSS::FillRule::Nonzero; }
    static CSS::ClipRule clip_rule() { return CSS::ClipRule::Nonzero; }
    static CSS::StrokeLinecap stroke_linecap() { return CSS::StrokeLinecap::Butt; }
    static CSS::StrokeLinejoin stroke_linejoin() { return CSS::StrokeLinejoin::Miter; }
    static float stroke_miterlimit() { return 4.0f; }
    static float stroke_opacity() { return 1.0f; }
    static float stop_opacity() { return 1.0f; }
    static CSS::TextAnchor text_anchor() { return CSS::TextAnchor::Start; }
    static CSS::Length border_radius() { return Length::make_px(0); }
    static Variant<CSS::VerticalAlign, CSS::LengthPercentage> vertical_align() { return CSS::VerticalAlign::Baseline; }
    static CSS::LengthBox inset() { return { CSS::Length::make_auto(), CSS::Length::make_auto(), CSS::Length::make_auto(), CSS::Length::make_auto() }; }
    static CSS::LengthBox margin() { return { CSS::Length::make_px(0), CSS::Length::make_px(0), CSS::Length::make_px(0), CSS::Length::make_px(0) }; }
    static CSS::LengthBox padding() { return { CSS::Length::make_px(0), CSS::Length::make_px(0), CSS::Length::make_px(0), CSS::Length::make_px(0) }; }
    static CSS::Size width() { return CSS::Size::make_auto(); }
    static CSS::Size min_width() { return CSS::Size::make_auto(); }
    static CSS::Size max_width() { return CSS::Size::make_none(); }
    static CSS::Size height() { return CSS::Size::make_auto(); }
    static CSS::Size min_height() { return CSS::Size::make_auto(); }
    static CSS::Size max_height() { return CSS::Size::make_none(); }
    static CSS::GridTrackSizeList grid_template_columns() { return CSS::GridTrackSizeList::make_none(); }
    static CSS::GridTrackSizeList grid_template_rows() { return CSS::GridTrackSizeList::make_none(); }
    static CSS::GridTrackPlacement grid_column_end() { return CSS::GridTrackPlacement::make_auto(); }
    static CSS::GridTrackPlacement grid_column_start() { return CSS::GridTrackPlacement::make_auto(); }
    static CSS::GridTrackPlacement grid_row_end() { return CSS::GridTrackPlacement::make_auto(); }
    static CSS::GridTrackPlacement grid_row_start() { return CSS::GridTrackPlacement::make_auto(); }
    static CSS::GridAutoFlow grid_auto_flow() { return CSS::GridAutoFlow {}; }
    static ColumnCount column_count() { return ColumnCount::make_auto(); }
    static Variant<LengthPercentage, NormalGap> column_gap() { return NormalGap {}; }
    static CSS::ColumnSpan column_span() { return CSS::ColumnSpan::None; }
    static CSS::Size column_width() { return CSS::Size::make_auto(); }
    static Variant<LengthPercentage, NormalGap> row_gap() { return NormalGap {}; }
    static CSS::BorderCollapse border_collapse() { return CSS::BorderCollapse::Separate; }
    static Vector<Vector<String>> grid_template_areas() { return {}; }
    static CSS::Time transition_delay() { return CSS::Time::make_seconds(0); }
    static CSS::ObjectFit object_fit() { return CSS::ObjectFit::Fill; }
    static CSS::ObjectPosition object_position() { return {}; }
    static Color outline_color() { return Color::Black; }
    static CSS::Length outline_offset() { return CSS::Length::make_px(0); }
    static CSS::OutlineStyle outline_style() { return CSS::OutlineStyle::None; }
    static CSS::Length outline_width() { return CSS::Length::make_px(3); }
    static CSS::TableLayout table_layout() { return CSS::TableLayout::Auto; }
    static QuotesData quotes() { return QuotesData { .type = QuotesData::Type::Auto }; }
    static CSS::TransformBox transform_box() { return CSS::TransformBox::ViewBox; }
    static CSS::Direction direction() { return CSS::Direction::Ltr; }
    static CSS::UnicodeBidi unicode_bidi() { return CSS::UnicodeBidi::Normal; }
    static CSS::WritingMode writing_mode() { return CSS::WritingMode::HorizontalTb; }

    // https://www.w3.org/TR/SVG/geometry.html
    static LengthPercentage cx() { return CSS::Length::make_px(0); }
    static LengthPercentage cy() { return CSS::Length::make_px(0); }
    static LengthPercentage r() { return CSS::Length::make_px(0); }
    static LengthPercentage rx() { return CSS::Length::make_auto(); }
    static LengthPercentage ry() { return CSS::Length::make_auto(); }
    static LengthPercentage x() { return CSS::Length::make_px(0); }
    static LengthPercentage y() { return CSS::Length::make_px(0); }

    static CSS::MaskType mask_type() { return CSS::MaskType::Luminance; }
    static CSS::MathShift math_shift() { return CSS::MathShift::Normal; }
    static CSS::MathStyle math_style() { return CSS::MathStyle::Normal; }
    static int math_depth() { return 0; }

    static CSS::ScrollbarWidth scrollbar_width() { return CSS::ScrollbarWidth::Auto; }
};

enum class BackgroundSize {
    Contain,
    Cover,
    LengthPercentage,
};

// https://svgwg.org/svg2-draft/painting.html#SpecifyingPaint
class SVGPaint {
public:
    SVGPaint(Color color)
        : m_value(color)
    {
    }
    SVGPaint(URL::URL const& url)
        : m_value(url)
    {
    }

    bool is_color() const { return m_value.has<Color>(); }
    bool is_url() const { return m_value.has<URL::URL>(); }
    Color as_color() const { return m_value.get<Color>(); }
    URL::URL const& as_url() const { return m_value.get<URL::URL>(); }

private:
    Variant<URL::URL, Color> m_value;
};

// https://drafts.fxtf.org/css-masking-1/#typedef-mask-reference
class MaskReference {
public:
    // TODO: Support other mask types.
    MaskReference(URL::URL const& url)
        : m_url(url)
    {
    }

    URL::URL const& url() const { return m_url; }

private:
    URL::URL m_url;
};

// https://drafts.fxtf.org/css-masking/#the-clip-path
// TODO: Support clip sources.
class ClipPathReference {
public:
    ClipPathReference(URL::URL const& url)
        : m_clip_source(url)
    {
    }

    ClipPathReference(BasicShapeStyleValue const& basic_shape)
        : m_clip_source(basic_shape)
    {
    }

    bool is_basic_shape() const { return m_clip_source.has<BasicShape>(); }

    bool is_url() const { return m_clip_source.has<URL::URL>(); }

    URL::URL const& url() const { return m_clip_source.get<URL::URL>(); }

    BasicShapeStyleValue const& basic_shape() const { return *m_clip_source.get<BasicShape>(); }

private:
    using BasicShape = NonnullRefPtr<BasicShapeStyleValue const>;

    Variant<URL::URL, BasicShape> m_clip_source;
};

struct BackgroundLayerData {
    RefPtr<CSS::AbstractImageStyleValue const> background_image { nullptr };
    CSS::BackgroundAttachment attachment { CSS::BackgroundAttachment::Scroll };
    CSS::BackgroundBox origin { CSS::BackgroundBox::PaddingBox };
    CSS::BackgroundBox clip { CSS::BackgroundBox::BorderBox };
    CSS::PositionEdge position_edge_x { CSS::PositionEdge::Left };
    CSS::LengthPercentage position_offset_x { CSS::Length::make_px(0) };
    CSS::PositionEdge position_edge_y { CSS::PositionEdge::Top };
    CSS::LengthPercentage position_offset_y { CSS::Length::make_px(0) };
    CSS::BackgroundSize size_type { CSS::BackgroundSize::LengthPercentage };
    CSS::LengthPercentage size_x { CSS::Length::make_auto() };
    CSS::LengthPercentage size_y { CSS::Length::make_auto() };
    CSS::Repeat repeat_x { CSS::Repeat::Repeat };
    CSS::Repeat repeat_y { CSS::Repeat::Repeat };
};

struct BorderData {
public:
    Color color { Color::Transparent };
    CSS::LineStyle line_style { CSS::LineStyle::None };
    CSSPixels width { 0 };

    bool operator==(BorderData const&) const = default;
};

struct TransformOrigin {
    CSS::LengthPercentage x { Percentage(50) };
    CSS::LengthPercentage y { Percentage(50) };
};

struct ShadowData {
    CSS::Length offset_x { Length::make_px(0) };
    CSS::Length offset_y { Length::make_px(0) };
    CSS::Length blur_radius { Length::make_px(0) };
    CSS::Length spread_distance { Length::make_px(0) };
    Color color {};
    CSS::ShadowPlacement placement { CSS::ShadowPlacement::Outer };
};

struct ContentData {
    enum class Type {
        Normal,
        None,
        String,
    } type { Type::Normal };

    // FIXME: Data is a list of identifiers, strings and image values.
    String data {};
    Optional<String> alt_text {};
};

struct CounterData {
    FlyString name;
    bool is_reversed;
    Optional<CounterValue> value;
};

struct BorderRadiusData {
    CSS::LengthPercentage horizontal_radius { InitialValues::border_radius() };
    CSS::LengthPercentage vertical_radius { InitialValues::border_radius() };
};

// FIXME: Find a better place for this helper.
inline Gfx::ScalingMode to_gfx_scaling_mode(CSS::ImageRendering css_value, Gfx::IntRect source, Gfx::IntRect target)
{
    switch (css_value) {
    case CSS::ImageRendering::Auto:
    case CSS::ImageRendering::HighQuality:
    case CSS::ImageRendering::Smooth:
        if (target.width() < source.width() || target.height() < source.height())
            return Gfx::ScalingMode::BoxSampling;
        return Gfx::ScalingMode::BilinearBlend;
    case CSS::ImageRendering::CrispEdges:
        return Gfx::ScalingMode::NearestNeighbor;
    case CSS::ImageRendering::Pixelated:
        return Gfx::ScalingMode::SmoothPixels;
    }
    VERIFY_NOT_REACHED();
}

class ComputedValues {
    AK_MAKE_NONCOPYABLE(ComputedValues);
    AK_MAKE_NONMOVABLE(ComputedValues);

public:
    ComputedValues() = default;
    ~ComputedValues() = default;

    AspectRatio aspect_ratio() const { return m_noninherited.aspect_ratio; }
    CSS::Float float_() const { return m_noninherited.float_; }
    CSS::Length border_spacing_horizontal() const { return m_inherited.border_spacing_horizontal; }
    CSS::Length border_spacing_vertical() const { return m_inherited.border_spacing_vertical; }
    CSS::CaptionSide caption_side() const { return m_inherited.caption_side; }
    CSS::Clear clear() const { return m_noninherited.clear; }
    CSS::Clip clip() const { return m_noninherited.clip; }
    CSS::ContentVisibility content_visibility() const { return m_inherited.content_visibility; }
    CSS::Cursor cursor() const { return m_inherited.cursor; }
    CSS::ContentData content() const { return m_noninherited.content; }
    CSS::PointerEvents pointer_events() const { return m_inherited.pointer_events; }
    CSS::Display display() const { return m_noninherited.display; }
    Optional<int> const& z_index() const { return m_noninherited.z_index; }
    Variant<LengthOrCalculated, NumberOrCalculated> tab_size() const { return m_inherited.tab_size; }
    CSS::TextAlign text_align() const { return m_inherited.text_align; }
    CSS::TextJustify text_justify() const { return m_inherited.text_justify; }
    CSS::LengthPercentage const& text_indent() const { return m_inherited.text_indent; }
    Vector<CSS::TextDecorationLine> const& text_decoration_line() const { return m_noninherited.text_decoration_line; }
    CSS::LengthPercentage const& text_decoration_thickness() const { return m_noninherited.text_decoration_thickness; }
    CSS::TextDecorationStyle text_decoration_style() const { return m_noninherited.text_decoration_style; }
    Color text_decoration_color() const { return m_noninherited.text_decoration_color; }
    CSS::TextTransform text_transform() const { return m_inherited.text_transform; }
    CSS::TextOverflow text_overflow() const { return m_noninherited.text_overflow; }
    Vector<ShadowData> const& text_shadow() const { return m_inherited.text_shadow; }
    CSS::Positioning position() const { return m_noninherited.position; }
    CSS::WhiteSpace white_space() const { return m_inherited.white_space; }
    CSS::LengthOrCalculated word_spacing() const { return m_inherited.word_spacing; }
    LengthOrCalculated letter_spacing() const { return m_inherited.letter_spacing; }
    CSS::FlexDirection flex_direction() const { return m_noninherited.flex_direction; }
    CSS::FlexWrap flex_wrap() const { return m_noninherited.flex_wrap; }
    FlexBasis const& flex_basis() const { return m_noninherited.flex_basis; }
    float flex_grow() const { return m_noninherited.flex_grow; }
    float flex_shrink() const { return m_noninherited.flex_shrink; }
    int order() const { return m_noninherited.order; }
    Optional<Color> accent_color() const { return m_inherited.accent_color; }
    CSS::AlignContent align_content() const { return m_noninherited.align_content; }
    CSS::AlignItems align_items() const { return m_noninherited.align_items; }
    CSS::AlignSelf align_self() const { return m_noninherited.align_self; }
    CSS::Appearance appearance() const { return m_noninherited.appearance; }
    float opacity() const { return m_noninherited.opacity; }
    CSS::Visibility visibility() const { return m_inherited.visibility; }
    CSS::ImageRendering image_rendering() const { return m_inherited.image_rendering; }
    CSS::JustifyContent justify_content() const { return m_noninherited.justify_content; }
    CSS::JustifySelf justify_self() const { return m_noninherited.justify_self; }
    CSS::JustifyItems justify_items() const { return m_noninherited.justify_items; }
    CSS::ResolvedFilter const& backdrop_filter() const { return m_noninherited.backdrop_filter; }
    CSS::ResolvedFilter const& filter() const { return m_noninherited.filter; }
    Vector<ShadowData> const& box_shadow() const { return m_noninherited.box_shadow; }
    CSS::BoxSizing box_sizing() const { return m_noninherited.box_sizing; }
    CSS::Size const& width() const { return m_noninherited.width; }
    CSS::Size const& min_width() const { return m_noninherited.min_width; }
    CSS::Size const& max_width() const { return m_noninherited.max_width; }
    CSS::Size const& height() const { return m_noninherited.height; }
    CSS::Size const& min_height() const { return m_noninherited.min_height; }
    CSS::Size const& max_height() const { return m_noninherited.max_height; }
    Variant<CSS::VerticalAlign, CSS::LengthPercentage> const& vertical_align() const { return m_noninherited.vertical_align; }
    CSS::GridTrackSizeList const& grid_auto_columns() const { return m_noninherited.grid_auto_columns; }
    CSS::GridTrackSizeList const& grid_auto_rows() const { return m_noninherited.grid_auto_rows; }
    CSS::GridAutoFlow const& grid_auto_flow() const { return m_noninherited.grid_auto_flow; }
    CSS::GridTrackSizeList const& grid_template_columns() const { return m_noninherited.grid_template_columns; }
    CSS::GridTrackSizeList const& grid_template_rows() const { return m_noninherited.grid_template_rows; }
    CSS::GridTrackPlacement const& grid_column_end() const { return m_noninherited.grid_column_end; }
    CSS::GridTrackPlacement const& grid_column_start() const { return m_noninherited.grid_column_start; }
    CSS::GridTrackPlacement const& grid_row_end() const { return m_noninherited.grid_row_end; }
    CSS::GridTrackPlacement const& grid_row_start() const { return m_noninherited.grid_row_start; }
    CSS::ColumnCount column_count() const { return m_noninherited.column_count; }
    Variant<LengthPercentage, NormalGap> const& column_gap() const { return m_noninherited.column_gap; }
    CSS::ColumnSpan const& column_span() const { return m_noninherited.column_span; }
    CSS::Size const& column_width() const { return m_noninherited.column_width; }
    Variant<LengthPercentage, NormalGap> const& row_gap() const { return m_noninherited.row_gap; }
    CSS::BorderCollapse border_collapse() const { return m_inherited.border_collapse; }
    Vector<Vector<String>> const& grid_template_areas() const { return m_noninherited.grid_template_areas; }
    CSS::ObjectFit object_fit() const { return m_noninherited.object_fit; }
    CSS::ObjectPosition object_position() const { return m_noninherited.object_position; }
    CSS::Direction direction() const { return m_inherited.direction; }
    CSS::UnicodeBidi unicode_bidi() const { return m_noninherited.unicode_bidi; }
    CSS::WritingMode writing_mode() const { return m_inherited.writing_mode; }

    CSS::LengthBox const& inset() const { return m_noninherited.inset; }
    const CSS::LengthBox& margin() const { return m_noninherited.margin; }
    const CSS::LengthBox& padding() const { return m_noninherited.padding; }

    BorderData const& border_left() const { return m_noninherited.border_left; }
    BorderData const& border_top() const { return m_noninherited.border_top; }
    BorderData const& border_right() const { return m_noninherited.border_right; }
    BorderData const& border_bottom() const { return m_noninherited.border_bottom; }

    const CSS::BorderRadiusData& border_bottom_left_radius() const { return m_noninherited.border_bottom_left_radius; }
    const CSS::BorderRadiusData& border_bottom_right_radius() const { return m_noninherited.border_bottom_right_radius; }
    const CSS::BorderRadiusData& border_top_left_radius() const { return m_noninherited.border_top_left_radius; }
    const CSS::BorderRadiusData& border_top_right_radius() const { return m_noninherited.border_top_right_radius; }

    CSS::Overflow overflow_x() const { return m_noninherited.overflow_x; }
    CSS::Overflow overflow_y() const { return m_noninherited.overflow_y; }

    Color color() const { return m_inherited.color; }
    Color background_color() const { return m_noninherited.background_color; }
    Vector<BackgroundLayerData> const& background_layers() const { return m_noninherited.background_layers; }

    Color webkit_text_fill_color() const { return m_inherited.webkit_text_fill_color; }

    CSS::ListStyleType list_style_type() const { return m_inherited.list_style_type; }
    CSS::ListStylePosition list_style_position() const { return m_inherited.list_style_position; }

    Optional<SVGPaint> const& fill() const { return m_inherited.fill; }
    CSS::FillRule fill_rule() const { return m_inherited.fill_rule; }
    Optional<SVGPaint> const& stroke() const { return m_inherited.stroke; }
    float fill_opacity() const { return m_inherited.fill_opacity; }
    CSS::StrokeLinecap stroke_linecap() const { return m_inherited.stroke_linecap; }
    CSS::StrokeLinejoin stroke_linejoin() const { return m_inherited.stroke_linejoin; }
    NumberOrCalculated stroke_miterlimit() const { return m_inherited.stroke_miterlimit; }
    float stroke_opacity() const { return m_inherited.stroke_opacity; }
    LengthPercentage const& stroke_width() const { return m_inherited.stroke_width; }
    Color stop_color() const { return m_noninherited.stop_color; }
    float stop_opacity() const { return m_noninherited.stop_opacity; }
    CSS::TextAnchor text_anchor() const { return m_inherited.text_anchor; }
    RefPtr<AbstractImageStyleValue const> mask_image() const { return m_noninherited.mask_image; }
    Optional<MaskReference> const& mask() const { return m_noninherited.mask; }
    CSS::MaskType mask_type() const { return m_noninherited.mask_type; }
    Optional<ClipPathReference> const& clip_path() const { return m_noninherited.clip_path; }
    CSS::ClipRule clip_rule() const { return m_inherited.clip_rule; }

    LengthPercentage const& cx() const { return m_noninherited.cx; }
    LengthPercentage const& cy() const { return m_noninherited.cy; }
    LengthPercentage const& r() const { return m_noninherited.r; }
    LengthPercentage const& rx() const { return m_noninherited.ry; }
    LengthPercentage const& ry() const { return m_noninherited.ry; }
    LengthPercentage const& x() const { return m_noninherited.x; }
    LengthPercentage const& y() const { return m_noninherited.y; }

    Vector<CSS::Transformation> const& transformations() const { return m_noninherited.transformations; }
    CSS::TransformBox const& transform_box() const { return m_noninherited.transform_box; }
    CSS::TransformOrigin const& transform_origin() const { return m_noninherited.transform_origin; }
    Optional<CSS::Transformation> const& rotate() const { return m_noninherited.rotate; }

    Gfx::FontCascadeList const& font_list() const { return *m_inherited.font_list; }
    CSSPixels font_size() const { return m_inherited.font_size; }
    int font_weight() const { return m_inherited.font_weight; }
    CSS::FontVariant font_variant() const { return m_inherited.font_variant; }
    Optional<FlyString> font_language_override() const { return m_inherited.font_language_override; }
    Optional<HashMap<FlyString, IntegerOrCalculated>> font_feature_settings() const { return m_inherited.font_feature_settings; }
    Optional<HashMap<FlyString, NumberOrCalculated>> font_variation_settings() const { return m_inherited.font_variation_settings; }
    CSSPixels line_height() const { return m_inherited.line_height; }
    CSS::Time transition_delay() const { return m_noninherited.transition_delay; }

    Color outline_color() const { return m_noninherited.outline_color; }
    CSS::Length outline_offset() const { return m_noninherited.outline_offset; }
    CSS::OutlineStyle outline_style() const { return m_noninherited.outline_style; }
    CSS::Length outline_width() const { return m_noninherited.outline_width; }

    CSS::TableLayout table_layout() const { return m_noninherited.table_layout; }

    CSS::QuotesData quotes() const { return m_inherited.quotes; }

    CSS::MathShift math_shift() const { return m_inherited.math_shift; }
    CSS::MathStyle math_style() const { return m_inherited.math_style; }
    int math_depth() const { return m_inherited.math_depth; }

    CSS::ScrollbarWidth scrollbar_width() const { return m_noninherited.scrollbar_width; }

    NonnullOwnPtr<ComputedValues> clone_inherited_values() const
    {
        auto clone = make<ComputedValues>();
        clone->m_inherited = m_inherited;
        return clone;
    }

protected:
    struct {
        RefPtr<Gfx::FontCascadeList> font_list {};
        CSSPixels font_size { InitialValues::font_size() };
        int font_weight { InitialValues::font_weight() };
        CSS::FontVariant font_variant { InitialValues::font_variant() };
        Optional<FlyString> font_language_override;
        Optional<HashMap<FlyString, IntegerOrCalculated>> font_feature_settings;
        Optional<HashMap<FlyString, NumberOrCalculated>> font_variation_settings;
        CSSPixels line_height { InitialValues::line_height() };
        CSS::BorderCollapse border_collapse { InitialValues::border_collapse() };
        CSS::Length border_spacing_horizontal { InitialValues::border_spacing() };
        CSS::Length border_spacing_vertical { InitialValues::border_spacing() };
        CSS::CaptionSide caption_side { InitialValues::caption_side() };
        Color color { InitialValues::color() };
        Optional<Color> accent_color {};
        Color webkit_text_fill_color { InitialValues::color() };
        CSS::ContentVisibility content_visibility { InitialValues::content_visibility() };
        CSS::Cursor cursor { InitialValues::cursor() };
        CSS::ImageRendering image_rendering { InitialValues::image_rendering() };
        CSS::PointerEvents pointer_events { InitialValues::pointer_events() };
        Variant<LengthOrCalculated, NumberOrCalculated> tab_size { InitialValues::tab_size() };
        CSS::TextAlign text_align { InitialValues::text_align() };
        CSS::TextJustify text_justify { InitialValues::text_justify() };
        CSS::TextTransform text_transform { InitialValues::text_transform() };
        CSS::LengthPercentage text_indent { InitialValues::text_indent() };
        CSS::WhiteSpace white_space { InitialValues::white_space() };
        CSS::WordBreak word_break { InitialValues::word_break() };
        CSS::LengthOrCalculated word_spacing { InitialValues::word_spacing() };
        LengthOrCalculated letter_spacing { InitialValues::letter_spacing() };
        CSS::ListStyleType list_style_type { InitialValues::list_style_type() };
        CSS::ListStylePosition list_style_position { InitialValues::list_style_position() };
        CSS::Visibility visibility { InitialValues::visibility() };
        CSS::QuotesData quotes { InitialValues::quotes() };
        CSS::Direction direction { InitialValues::direction() };
        CSS::WritingMode writing_mode { InitialValues::writing_mode() };

        Optional<SVGPaint> fill;
        CSS::FillRule fill_rule { InitialValues::fill_rule() };
        Optional<SVGPaint> stroke;
        float fill_opacity { InitialValues::fill_opacity() };
        CSS::StrokeLinecap stroke_linecap { InitialValues::stroke_linecap() };
        CSS::StrokeLinejoin stroke_linejoin { InitialValues::stroke_linejoin() };
        NumberOrCalculated stroke_miterlimit { InitialValues::stroke_miterlimit() };
        float stroke_opacity { InitialValues::stroke_opacity() };
        LengthPercentage stroke_width { Length::make_px(1) };
        CSS::TextAnchor text_anchor { InitialValues::text_anchor() };
        CSS::ClipRule clip_rule { InitialValues::clip_rule() };

        Vector<ShadowData> text_shadow;

        CSS::MathShift math_shift { InitialValues::math_shift() };
        CSS::MathStyle math_style { InitialValues::math_style() };
        int math_depth { InitialValues::math_depth() };
    } m_inherited;

    struct {
        AspectRatio aspect_ratio { InitialValues::aspect_ratio() };
        CSS::Float float_ { InitialValues::float_() };
        CSS::Clear clear { InitialValues::clear() };
        CSS::Clip clip { InitialValues::clip() };
        CSS::Display display { InitialValues::display() };
        Optional<int> z_index;
        // FIXME: Store this as flags in a u8.
        Vector<CSS::TextDecorationLine> text_decoration_line { InitialValues::text_decoration_line() };
        CSS::LengthPercentage text_decoration_thickness { InitialValues::text_decoration_thickness() };
        CSS::TextDecorationStyle text_decoration_style { InitialValues::text_decoration_style() };
        Color text_decoration_color { InitialValues::color() };
        CSS::TextOverflow text_overflow { InitialValues::text_overflow() };
        CSS::Positioning position { InitialValues::position() };
        CSS::Size width { InitialValues::width() };
        CSS::Size min_width { InitialValues::min_width() };
        CSS::Size max_width { InitialValues::max_width() };
        CSS::Size height { InitialValues::height() };
        CSS::Size min_height { InitialValues::min_height() };
        CSS::Size max_height { InitialValues::max_height() };
        CSS::LengthBox inset { InitialValues::inset() };
        CSS::LengthBox margin { InitialValues::margin() };
        CSS::LengthBox padding { InitialValues::padding() };
        CSS::ResolvedFilter backdrop_filter { InitialValues::backdrop_filter() };
        CSS::ResolvedFilter filter { InitialValues::filter() };
        BorderData border_left;
        BorderData border_top;
        BorderData border_right;
        BorderData border_bottom;
        BorderRadiusData border_bottom_left_radius;
        BorderRadiusData border_bottom_right_radius;
        BorderRadiusData border_top_left_radius;
        BorderRadiusData border_top_right_radius;
        Color background_color { InitialValues::background_color() };
        Vector<BackgroundLayerData> background_layers;
        CSS::FlexDirection flex_direction { InitialValues::flex_direction() };
        CSS::FlexWrap flex_wrap { InitialValues::flex_wrap() };
        CSS::FlexBasis flex_basis { InitialValues::flex_basis() };
        float flex_grow { InitialValues::flex_grow() };
        float flex_shrink { InitialValues::flex_shrink() };
        int order { InitialValues::order() };
        CSS::AlignContent align_content { InitialValues::align_content() };
        CSS::AlignItems align_items { InitialValues::align_items() };
        CSS::AlignSelf align_self { InitialValues::align_self() };
        CSS::Appearance appearance { InitialValues::appearance() };
        CSS::JustifyContent justify_content { InitialValues::justify_content() };
        CSS::JustifyItems justify_items { InitialValues::justify_items() };
        CSS::JustifySelf justify_self { InitialValues::justify_self() };
        CSS::Overflow overflow_x { InitialValues::overflow() };
        CSS::Overflow overflow_y { InitialValues::overflow() };
        float opacity { InitialValues::opacity() };
        Vector<ShadowData> box_shadow {};
        Vector<CSS::Transformation> transformations {};
        CSS::TransformBox transform_box { InitialValues::transform_box() };
        CSS::TransformOrigin transform_origin {};
        CSS::BoxSizing box_sizing { InitialValues::box_sizing() };
        CSS::ContentData content;
        Variant<CSS::VerticalAlign, CSS::LengthPercentage> vertical_align { InitialValues::vertical_align() };
        CSS::GridTrackSizeList grid_auto_columns;
        CSS::GridTrackSizeList grid_auto_rows;
        CSS::GridTrackSizeList grid_template_columns;
        CSS::GridTrackSizeList grid_template_rows;
        CSS::GridAutoFlow grid_auto_flow { InitialValues::grid_auto_flow() };
        CSS::GridTrackPlacement grid_column_end { InitialValues::grid_column_end() };
        CSS::GridTrackPlacement grid_column_start { InitialValues::grid_column_start() };
        CSS::GridTrackPlacement grid_row_end { InitialValues::grid_row_end() };
        CSS::GridTrackPlacement grid_row_start { InitialValues::grid_row_start() };
        CSS::ColumnCount column_count { InitialValues::column_count() };
        Variant<LengthPercentage, NormalGap> column_gap { InitialValues::column_gap() };
        CSS::ColumnSpan column_span { InitialValues::column_span() };
        CSS::Size column_width { InitialValues::column_width() };
        Variant<LengthPercentage, NormalGap> row_gap { InitialValues::row_gap() };
        Vector<Vector<String>> grid_template_areas { InitialValues::grid_template_areas() };
        Gfx::Color stop_color { InitialValues::stop_color() };
        float stop_opacity { InitialValues::stop_opacity() };
        CSS::Time transition_delay { InitialValues::transition_delay() };
        Color outline_color { InitialValues::outline_color() };
        CSS::Length outline_offset { InitialValues::outline_offset() };
        CSS::OutlineStyle outline_style { InitialValues::outline_style() };
        CSS::Length outline_width { InitialValues::outline_width() };
        CSS::TableLayout table_layout { InitialValues::table_layout() };
        CSS::ObjectFit object_fit { InitialValues::object_fit() };
        CSS::ObjectPosition object_position { InitialValues::object_position() };
        CSS::UnicodeBidi unicode_bidi { InitialValues::unicode_bidi() };
        Optional<CSS::Transformation> rotate;

        Optional<MaskReference> mask;
        CSS::MaskType mask_type { InitialValues::mask_type() };
        Optional<ClipPathReference> clip_path;
        RefPtr<CSS::AbstractImageStyleValue> mask_image;

        LengthPercentage cx { InitialValues::cx() };
        LengthPercentage cy { InitialValues::cy() };
        LengthPercentage r { InitialValues::r() };
        LengthPercentage rx { InitialValues::rx() };
        LengthPercentage ry { InitialValues::ry() };
        LengthPercentage x { InitialValues::x() };
        LengthPercentage y { InitialValues::x() };

        CSS::ScrollbarWidth scrollbar_width { InitialValues::scrollbar_width() };
        Vector<CounterData, 0> counter_increment;
        Vector<CounterData, 0> counter_reset;
        Vector<CounterData, 0> counter_set;
    } m_noninherited;
};

class ImmutableComputedValues final : public ComputedValues {
};

class MutableComputedValues final : public ComputedValues {
public:
    void inherit_from(ComputedValues const& other)
    {
        m_inherited = static_cast<MutableComputedValues const&>(other).m_inherited;
    }

    void set_aspect_ratio(AspectRatio aspect_ratio) { m_noninherited.aspect_ratio = aspect_ratio; }
    void set_font_list(NonnullRefPtr<Gfx::FontCascadeList> font_list) { m_inherited.font_list = move(font_list); }
    void set_font_size(CSSPixels font_size) { m_inherited.font_size = font_size; }
    void set_font_weight(int font_weight) { m_inherited.font_weight = font_weight; }
    void set_font_variant(CSS::FontVariant font_variant) { m_inherited.font_variant = font_variant; }
    void set_font_language_override(Optional<FlyString> font_language_override) { m_inherited.font_language_override = font_language_override; }
    void set_font_feature_settings(Optional<HashMap<FlyString, IntegerOrCalculated>> value) { m_inherited.font_feature_settings = move(value); }
    void set_font_variation_settings(Optional<HashMap<FlyString, NumberOrCalculated>> value) { m_inherited.font_variation_settings = move(value); }
    void set_line_height(CSSPixels line_height) { m_inherited.line_height = line_height; }
    void set_border_spacing_horizontal(CSS::Length border_spacing_horizontal) { m_inherited.border_spacing_horizontal = border_spacing_horizontal; }
    void set_border_spacing_vertical(CSS::Length border_spacing_vertical) { m_inherited.border_spacing_vertical = border_spacing_vertical; }
    void set_caption_side(CSS::CaptionSide caption_side) { m_inherited.caption_side = caption_side; }
    void set_color(Color color) { m_inherited.color = color; }
    void set_clip(CSS::Clip const& clip) { m_noninherited.clip = clip; }
    void set_content(ContentData const& content) { m_noninherited.content = content; }
    void set_content_visibility(CSS::ContentVisibility content_visibility) { m_inherited.content_visibility = content_visibility; }
    void set_cursor(CSS::Cursor cursor) { m_inherited.cursor = cursor; }
    void set_image_rendering(CSS::ImageRendering value) { m_inherited.image_rendering = value; }
    void set_pointer_events(CSS::PointerEvents value) { m_inherited.pointer_events = value; }
    void set_background_color(Color color) { m_noninherited.background_color = color; }
    void set_background_layers(Vector<BackgroundLayerData>&& layers) { m_noninherited.background_layers = move(layers); }
    void set_float(CSS::Float value) { m_noninherited.float_ = value; }
    void set_clear(CSS::Clear value) { m_noninherited.clear = value; }
    void set_z_index(Optional<int> value) { m_noninherited.z_index = value; }
    void set_tab_size(Variant<LengthOrCalculated, NumberOrCalculated> value) { m_inherited.tab_size = value; }
    void set_text_align(CSS::TextAlign text_align) { m_inherited.text_align = text_align; }
    void set_text_justify(CSS::TextJustify text_justify) { m_inherited.text_justify = text_justify; }
    void set_text_decoration_line(Vector<CSS::TextDecorationLine> value) { m_noninherited.text_decoration_line = move(value); }
    void set_text_decoration_thickness(CSS::LengthPercentage value) { m_noninherited.text_decoration_thickness = move(value); }
    void set_text_decoration_style(CSS::TextDecorationStyle value) { m_noninherited.text_decoration_style = value; }
    void set_text_decoration_color(Color value) { m_noninherited.text_decoration_color = value; }
    void set_text_transform(CSS::TextTransform value) { m_inherited.text_transform = value; }
    void set_text_shadow(Vector<ShadowData>&& value) { m_inherited.text_shadow = move(value); }
    void set_text_indent(CSS::LengthPercentage value) { m_inherited.text_indent = move(value); }
    void set_text_overflow(CSS::TextOverflow value) { m_noninherited.text_overflow = value; }
    void set_webkit_text_fill_color(Color value) { m_inherited.webkit_text_fill_color = value; }
    void set_position(CSS::Positioning position) { m_noninherited.position = position; }
    void set_white_space(CSS::WhiteSpace value) { m_inherited.white_space = value; }
    void set_word_spacing(CSS::LengthOrCalculated value) { m_inherited.word_spacing = value; }
    void set_word_break(CSS::WordBreak value) { m_inherited.word_break = value; }
    void set_letter_spacing(CSS::LengthOrCalculated value) { m_inherited.letter_spacing = value; }
    void set_width(CSS::Size const& width) { m_noninherited.width = width; }
    void set_min_width(CSS::Size const& width) { m_noninherited.min_width = width; }
    void set_max_width(CSS::Size const& width) { m_noninherited.max_width = width; }
    void set_height(CSS::Size const& height) { m_noninherited.height = height; }
    void set_min_height(CSS::Size const& height) { m_noninherited.min_height = height; }
    void set_max_height(CSS::Size const& height) { m_noninherited.max_height = height; }
    void set_inset(CSS::LengthBox const& inset) { m_noninherited.inset = inset; }
    void set_margin(const CSS::LengthBox& margin) { m_noninherited.margin = margin; }
    void set_padding(const CSS::LengthBox& padding) { m_noninherited.padding = padding; }
    void set_overflow_x(CSS::Overflow value) { m_noninherited.overflow_x = value; }
    void set_overflow_y(CSS::Overflow value) { m_noninherited.overflow_y = value; }
    void set_list_style_type(CSS::ListStyleType value) { m_inherited.list_style_type = value; }
    void set_list_style_position(CSS::ListStylePosition value) { m_inherited.list_style_position = value; }
    void set_display(CSS::Display value) { m_noninherited.display = value; }
    void set_backdrop_filter(CSS::ResolvedFilter backdrop_filter) { m_noninherited.backdrop_filter = move(backdrop_filter); }
    void set_filter(CSS::ResolvedFilter filter) { m_noninherited.filter = move(filter); }
    void set_border_bottom_left_radius(CSS::BorderRadiusData value) { m_noninherited.border_bottom_left_radius = move(value); }
    void set_border_bottom_right_radius(CSS::BorderRadiusData value) { m_noninherited.border_bottom_right_radius = move(value); }
    void set_border_top_left_radius(CSS::BorderRadiusData value) { m_noninherited.border_top_left_radius = move(value); }
    void set_border_top_right_radius(CSS::BorderRadiusData value) { m_noninherited.border_top_right_radius = move(value); }
    BorderData& border_left() { return m_noninherited.border_left; }
    BorderData& border_top() { return m_noninherited.border_top; }
    BorderData& border_right() { return m_noninherited.border_right; }
    BorderData& border_bottom() { return m_noninherited.border_bottom; }
    void set_flex_direction(CSS::FlexDirection value) { m_noninherited.flex_direction = value; }
    void set_flex_wrap(CSS::FlexWrap value) { m_noninherited.flex_wrap = value; }
    void set_flex_basis(FlexBasis value) { m_noninherited.flex_basis = move(value); }
    void set_flex_grow(float value) { m_noninherited.flex_grow = value; }
    void set_flex_shrink(float value) { m_noninherited.flex_shrink = value; }
    void set_order(int value) { m_noninherited.order = value; }
    void set_accent_color(Color value) { m_inherited.accent_color = value; }
    void set_align_content(CSS::AlignContent value) { m_noninherited.align_content = value; }
    void set_align_items(CSS::AlignItems value) { m_noninherited.align_items = value; }
    void set_align_self(CSS::AlignSelf value) { m_noninherited.align_self = value; }
    void set_appearance(CSS::Appearance value) { m_noninherited.appearance = value; }
    void set_opacity(float value) { m_noninherited.opacity = value; }
    void set_justify_content(CSS::JustifyContent value) { m_noninherited.justify_content = value; }
    void set_justify_items(CSS::JustifyItems value) { m_noninherited.justify_items = value; }
    void set_justify_self(CSS::JustifySelf value) { m_noninherited.justify_self = value; }
    void set_box_shadow(Vector<ShadowData>&& value) { m_noninherited.box_shadow = move(value); }
    void set_rotate(CSS::Transformation value) { m_noninherited.rotate = value; }
    void set_transformations(Vector<CSS::Transformation> value) { m_noninherited.transformations = move(value); }
    void set_transform_box(CSS::TransformBox value) { m_noninherited.transform_box = value; }
    void set_transform_origin(CSS::TransformOrigin value) { m_noninherited.transform_origin = value; }
    void set_box_sizing(CSS::BoxSizing value) { m_noninherited.box_sizing = value; }
    void set_vertical_align(Variant<CSS::VerticalAlign, CSS::LengthPercentage> value) { m_noninherited.vertical_align = move(value); }
    void set_visibility(CSS::Visibility value) { m_inherited.visibility = value; }
    void set_grid_auto_columns(CSS::GridTrackSizeList value) { m_noninherited.grid_auto_columns = move(value); }
    void set_grid_auto_rows(CSS::GridTrackSizeList value) { m_noninherited.grid_auto_rows = move(value); }
    void set_grid_template_columns(CSS::GridTrackSizeList value) { m_noninherited.grid_template_columns = move(value); }
    void set_grid_template_rows(CSS::GridTrackSizeList value) { m_noninherited.grid_template_rows = move(value); }
    void set_grid_column_end(CSS::GridTrackPlacement value) { m_noninherited.grid_column_end = value; }
    void set_grid_column_start(CSS::GridTrackPlacement value) { m_noninherited.grid_column_start = value; }
    void set_grid_row_end(CSS::GridTrackPlacement value) { m_noninherited.grid_row_end = value; }
    void set_grid_row_start(CSS::GridTrackPlacement value) { m_noninherited.grid_row_start = value; }
    void set_column_count(CSS::ColumnCount value) { m_noninherited.column_count = value; }
    void set_column_gap(Variant<LengthPercentage, NormalGap> const& column_gap) { m_noninherited.column_gap = column_gap; }
    void set_column_span(CSS::ColumnSpan const& column_span) { m_noninherited.column_span = column_span; }
    void set_column_width(CSS::Size const& column_width) { m_noninherited.column_width = column_width; }
    void set_row_gap(Variant<LengthPercentage, NormalGap> const& row_gap) { m_noninherited.row_gap = row_gap; }
    void set_border_collapse(CSS::BorderCollapse const& border_collapse) { m_inherited.border_collapse = border_collapse; }
    void set_grid_template_areas(Vector<Vector<String>> const& grid_template_areas) { m_noninherited.grid_template_areas = grid_template_areas; }
    void set_grid_auto_flow(CSS::GridAutoFlow grid_auto_flow) { m_noninherited.grid_auto_flow = grid_auto_flow; }
    void set_transition_delay(CSS::Time const& transition_delay) { m_noninherited.transition_delay = transition_delay; }
    void set_table_layout(CSS::TableLayout value) { m_noninherited.table_layout = value; }
    void set_quotes(CSS::QuotesData value) { m_inherited.quotes = value; }
    void set_object_fit(CSS::ObjectFit value) { m_noninherited.object_fit = value; }
    void set_object_position(CSS::ObjectPosition value) { m_noninherited.object_position = value; }
    void set_direction(CSS::Direction value) { m_inherited.direction = value; }
    void set_unicode_bidi(CSS::UnicodeBidi value) { m_noninherited.unicode_bidi = value; }
    void set_writing_mode(CSS::WritingMode value) { m_inherited.writing_mode = value; }

    void set_fill(SVGPaint value) { m_inherited.fill = value; }
    void set_stroke(SVGPaint value) { m_inherited.stroke = value; }
    void set_fill_rule(CSS::FillRule value) { m_inherited.fill_rule = value; }
    void set_fill_opacity(float value) { m_inherited.fill_opacity = value; }
    void set_stroke_linecap(CSS::StrokeLinecap value) { m_inherited.stroke_linecap = value; }
    void set_stroke_linejoin(CSS::StrokeLinejoin value) { m_inherited.stroke_linejoin = value; }
    void set_stroke_miterlimit(NumberOrCalculated value) { m_inherited.stroke_miterlimit = value; }
    void set_stroke_opacity(float value) { m_inherited.stroke_opacity = value; }
    void set_stroke_width(LengthPercentage value) { m_inherited.stroke_width = value; }
    void set_stop_color(Color value) { m_noninherited.stop_color = value; }
    void set_stop_opacity(float value) { m_noninherited.stop_opacity = value; }
    void set_text_anchor(CSS::TextAnchor value) { m_inherited.text_anchor = value; }
    void set_outline_color(Color value) { m_noninherited.outline_color = value; }
    void set_outline_offset(CSS::Length value) { m_noninherited.outline_offset = value; }
    void set_outline_style(CSS::OutlineStyle value) { m_noninherited.outline_style = value; }
    void set_outline_width(CSS::Length value) { m_noninherited.outline_width = value; }
    void set_mask(MaskReference value) { m_noninherited.mask = value; }
    void set_mask_type(CSS::MaskType value) { m_noninherited.mask_type = value; }
    void set_mask_image(CSS::AbstractImageStyleValue const& value) { m_noninherited.mask_image = value; }
    void set_clip_path(ClipPathReference value) { m_noninherited.clip_path = value; }
    void set_clip_rule(CSS::ClipRule value) { m_inherited.clip_rule = value; }

    void set_cx(LengthPercentage cx) { m_noninherited.cx = cx; }
    void set_cy(LengthPercentage cy) { m_noninherited.cy = cy; }
    void set_r(LengthPercentage r) { m_noninherited.r = r; }
    void set_rx(LengthPercentage rx) { m_noninherited.rx = rx; }
    void set_ry(LengthPercentage ry) { m_noninherited.ry = ry; }
    void set_x(LengthPercentage x) { m_noninherited.x = x; }
    void set_y(LengthPercentage y) { m_noninherited.y = y; }

    void set_math_shift(CSS::MathShift value) { m_inherited.math_shift = value; }
    void set_math_style(CSS::MathStyle value) { m_inherited.math_style = value; }
    void set_math_depth(int value) { m_inherited.math_depth = value; }

    void set_scrollbar_width(CSS::ScrollbarWidth value) { m_noninherited.scrollbar_width = value; }

    void set_counter_increment(Vector<CounterData> value) { m_noninherited.counter_increment = move(value); }
    void set_counter_reset(Vector<CounterData> value) { m_noninherited.counter_reset = move(value); }
    void set_counter_set(Vector<CounterData> value) { m_noninherited.counter_set = move(value); }
};

}
