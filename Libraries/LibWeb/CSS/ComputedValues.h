/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Font/FontVariant.h>
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
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/Ratio.h>
#include <LibWeb/CSS/Size.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/BasicShapeStyleValue.h>
#include <LibWeb/CSS/StyleValues/CursorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/Transformation.h>
#include <LibWeb/CSS/URL.h>

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

struct ObjectPosition {
    PositionEdge edge_x { PositionEdge::Left };
    LengthPercentage offset_x { Percentage(50) };
    PositionEdge edge_y { PositionEdge::Top };
    LengthPercentage offset_y { Percentage(50) };
};

// https://drafts.csswg.org/css-contain-2/#containment-types
struct Containment {
    bool size_containment : 1 { false };
    bool inline_size_containment : 1 { false };
    bool layout_containment : 1 { false };
    bool style_containment : 1 { false };
    bool paint_containment : 1 { false };

    bool is_empty() const { return !(size_containment || inline_size_containment || layout_containment || style_containment || paint_containment); }
};

struct ContainerType {
    bool is_size_container : 1 { false };
    bool is_inline_size_container : 1 { false };
    bool is_scroll_state_container : 1 { false };

    bool is_empty() const { return !(is_size_container || is_inline_size_container || is_scroll_state_container); }
};

struct ScrollbarColorData {
    Color thumb_color { Color::Transparent };
    Color track_color { Color::Transparent };
};

struct TextUnderlinePosition {
    TextUnderlinePositionHorizontal horizontal { TextUnderlinePositionHorizontal::Auto };
    TextUnderlinePositionVertical vertical { TextUnderlinePositionVertical::Auto };
};

struct WillChange {
    enum class Type : u8 {
        Contents,
        ScrollPosition,
    };
    using WillChangeEntry = Variant<Type, PropertyID>;

    WillChange(Vector<WillChangeEntry> values)
        : m_value(move(values))
    {
    }

    static WillChange make_auto() { return WillChange(); }

    bool is_auto() const { return m_value.is_empty(); }
    bool has_contents() const { return m_value.contains_slow(Type::Contents); }
    bool has_scroll_position() const { return m_value.contains_slow(Type::ScrollPosition); }
    bool has_property(PropertyID property_id) const { return m_value.contains_slow(property_id); }

private:
    WillChange()
    {
    }

    Vector<WillChangeEntry> m_value;
};

using CursorData = Variant<NonnullRefPtr<CursorStyleValue const>, CursorPredefined>;

using ListStyleType = Variant<CounterStyleNameKeyword, String>;

using PaintOrderList = Array<PaintOrder, 3>;

class InitialValues {
public:
    static AspectRatio aspect_ratio() { return AspectRatio { true, {} }; }
    static CSSPixels font_size() { return 16; }
    static FontKerning font_kerning() { return FontKerning::Auto; }
    static double font_weight() { return 400; }
    static CSSPixels line_height() { return 0; }
    static Float float_() { return Float::None; }
    static Length border_spacing() { return Length::make_px(0); }
    static CaptionSide caption_side() { return CaptionSide::Top; }
    static Color caret_color() { return Color::Black; }
    static Clear clear() { return Clear::None; }
    static Clip clip() { return Clip::make_auto(); }
    static ColorInterpolation color_interpolation() { return ColorInterpolation::Auto; }
    static PreferredColorScheme color_scheme() { return PreferredColorScheme::Auto; }
    static ContentVisibility content_visibility() { return ContentVisibility::Visible; }
    static CursorData cursor() { return { CursorPredefined::Auto }; }
    static WhiteSpaceCollapse white_space_collapse() { return WhiteSpaceCollapse::Collapse; }
    static WordBreak word_break() { return WordBreak::Normal; }
    static CSSPixels word_spacing() { return 0; }
    static CSSPixels letter_spacing() { return 0; }
    static Variant<LengthOrCalculated, NumberOrCalculated> tab_size() { return NumberOrCalculated(8.0f); }
    static TextAlign text_align() { return TextAlign::Start; }
    static TextJustify text_justify() { return TextJustify::Auto; }
    static Positioning position() { return Positioning::Static; }
    static TextDecorationLine text_decoration_line() { return TextDecorationLine::None; }
    static TextDecorationStyle text_decoration_style() { return TextDecorationStyle::Solid; }
    static TextTransform text_transform() { return TextTransform::None; }
    static TextOverflow text_overflow() { return TextOverflow::Clip; }
    static LengthPercentage text_indent() { return Length::make_px(0); }
    static TextWrapMode text_wrap_mode() { return TextWrapMode::Wrap; }
    static TextRendering text_rendering() { return TextRendering::Auto; }
    static CSSPixels text_underline_offset() { return 2; }
    static TextUnderlinePosition text_underline_position() { return { .horizontal = TextUnderlinePositionHorizontal::Auto, .vertical = TextUnderlinePositionVertical::Auto }; }
    static Display display() { return Display { DisplayOutside::Inline, DisplayInside::Flow }; }
    static Color color() { return Color::Black; }
    static Color stop_color() { return Color::Black; }
    static Filter backdrop_filter() { return Filter::make_none(); }
    static Filter filter() { return Filter::make_none(); }
    static Color background_color() { return Color::Transparent; }
    static ListStyleType list_style_type() { return CounterStyleNameKeyword::Disc; }
    static ListStylePosition list_style_position() { return ListStylePosition::Outside; }
    static Visibility visibility() { return Visibility::Visible; }
    static FlexDirection flex_direction() { return FlexDirection::Row; }
    static FlexWrap flex_wrap() { return FlexWrap::Nowrap; }
    static FlexBasis flex_basis() { return Size::make_auto(); }
    static ImageRendering image_rendering() { return ImageRendering::Auto; }
    static JustifyContent justify_content() { return JustifyContent::FlexStart; }
    static JustifyItems justify_items() { return JustifyItems::Legacy; }
    static JustifySelf justify_self() { return JustifySelf::Auto; }
    static AlignContent align_content() { return AlignContent::Stretch; }
    static AlignItems align_items() { return AlignItems::Stretch; }
    static AlignSelf align_self() { return AlignSelf::Auto; }
    static Appearance appearance() { return Appearance::Auto; }
    static Overflow overflow() { return Overflow::Visible; }
    static BoxSizing box_sizing() { return BoxSizing::ContentBox; }
    static PointerEvents pointer_events() { return PointerEvents::Auto; }
    static float flex_grow() { return 0.0f; }
    static float flex_shrink() { return 1.0f; }
    static int order() { return 0; }
    static float opacity() { return 1.0f; }
    static float fill_opacity() { return 1.0f; }
    static FillRule fill_rule() { return FillRule::Nonzero; }
    static ClipRule clip_rule() { return ClipRule::Nonzero; }
    static Color flood_color() { return Color::Black; }
    static float flood_opacity() { return 1.0f; }
    static LengthPercentage stroke_dashoffset() { return Length::make_px(0); }
    static StrokeLinecap stroke_linecap() { return StrokeLinecap::Butt; }
    static StrokeLinejoin stroke_linejoin() { return StrokeLinejoin::Miter; }
    static float stroke_miterlimit() { return 4.0f; }
    static float stroke_opacity() { return 1.0f; }
    static LengthPercentage stroke_width() { return Length::make_px(1); }
    static float stop_opacity() { return 1.0f; }
    static TextAnchor text_anchor() { return TextAnchor::Start; }
    static Length border_radius() { return Length::make_px(0); }
    static Variant<VerticalAlign, LengthPercentage> vertical_align() { return VerticalAlign::Baseline; }
    static LengthBox inset() { return {}; }
    static LengthBox margin() { return { Length::make_px(0), Length::make_px(0), Length::make_px(0), Length::make_px(0) }; }
    static LengthBox padding() { return { Length::make_px(0), Length::make_px(0), Length::make_px(0), Length::make_px(0) }; }
    static Size width() { return Size::make_auto(); }
    static Size min_width() { return Size::make_auto(); }
    static Size max_width() { return Size::make_none(); }
    static Size height() { return Size::make_auto(); }
    static Size min_height() { return Size::make_auto(); }
    static Size max_height() { return Size::make_none(); }
    static GridTrackSizeList grid_template_columns() { return GridTrackSizeList::make_none(); }
    static GridTrackSizeList grid_template_rows() { return GridTrackSizeList::make_none(); }
    static GridTrackPlacement grid_column_end() { return GridTrackPlacement::make_auto(); }
    static GridTrackPlacement grid_column_start() { return GridTrackPlacement::make_auto(); }
    static GridTrackPlacement grid_row_end() { return GridTrackPlacement::make_auto(); }
    static GridTrackPlacement grid_row_start() { return GridTrackPlacement::make_auto(); }
    static GridAutoFlow grid_auto_flow() { return GridAutoFlow {}; }
    static ColumnCount column_count() { return ColumnCount::make_auto(); }
    static Variant<LengthPercentage, NormalGap> column_gap() { return NormalGap {}; }
    static ColumnSpan column_span() { return ColumnSpan::None; }
    static Size column_width() { return Size::make_auto(); }
    static Size column_height() { return Size::make_auto(); }
    static Variant<LengthPercentage, NormalGap> row_gap() { return NormalGap {}; }
    static BorderCollapse border_collapse() { return BorderCollapse::Separate; }
    static EmptyCells empty_cells() { return EmptyCells::Show; }
    static Vector<Vector<String>> grid_template_areas() { return {}; }
    static Time transition_delay() { return Time::make_seconds(0); }
    static ObjectFit object_fit() { return ObjectFit::Fill; }
    static ObjectPosition object_position() { return {}; }
    static Color outline_color() { return Color::Black; }
    static Length outline_offset() { return Length::make_px(0); }
    static OutlineStyle outline_style() { return OutlineStyle::None; }
    static CSSPixels outline_width() { return 3; }
    static TableLayout table_layout() { return TableLayout::Auto; }
    static QuotesData quotes() { return QuotesData { .type = QuotesData::Type::Auto }; }
    static TransformBox transform_box() { return TransformBox::ViewBox; }
    static Direction direction() { return Direction::Ltr; }
    static UnicodeBidi unicode_bidi() { return UnicodeBidi::Normal; }
    static WritingMode writing_mode() { return WritingMode::HorizontalTb; }
    static UserSelect user_select() { return UserSelect::Auto; }
    static Isolation isolation() { return Isolation::Auto; }
    static Containment contain() { return {}; }
    static ContainerType container_type() { return {}; }
    static MixBlendMode mix_blend_mode() { return MixBlendMode::Normal; }
    static Optional<int> z_index() { return OptionalNone(); }

    // https://www.w3.org/TR/SVG/geometry.html
    static LengthPercentage cx() { return Length::make_px(0); }
    static LengthPercentage cy() { return Length::make_px(0); }
    static LengthPercentage r() { return Length::make_px(0); }
    static LengthPercentageOrAuto rx() { return LengthPercentageOrAuto::make_auto(); }
    static LengthPercentageOrAuto ry() { return LengthPercentageOrAuto::make_auto(); }
    static LengthPercentage x() { return Length::make_px(0); }
    static LengthPercentage y() { return Length::make_px(0); }

    static MaskType mask_type() { return MaskType::Luminance; }
    static MathShift math_shift() { return MathShift::Normal; }
    static MathStyle math_style() { return MathStyle::Normal; }
    static int math_depth() { return 0; }

    static ScrollbarColorData scrollbar_color()
    {
        return ScrollbarColorData {
            .thumb_color = Color(Color::NamedColor::DarkGray).with_alpha(192),
            .track_color = Color(Color::NamedColor::WarmGray).with_alpha(192),
        };
    }
    static ScrollbarWidth scrollbar_width() { return ScrollbarWidth::Auto; }
    static ShapeRendering shape_rendering() { return ShapeRendering::Auto; }
    static PaintOrderList paint_order() { return { PaintOrder::Fill, PaintOrder::Stroke, PaintOrder::Markers }; }
    static WillChange will_change() { return WillChange::make_auto(); }
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
    SVGPaint(URL const& url)
        : m_value(url)
    {
    }

    bool is_color() const { return m_value.has<Color>(); }
    bool is_url() const { return m_value.has<URL>(); }
    Color as_color() const { return m_value.get<Color>(); }
    URL const& as_url() const { return m_value.get<URL>(); }

private:
    Variant<URL, Color> m_value;
};

// https://drafts.fxtf.org/css-masking-1/#typedef-mask-reference
class MaskReference {
public:
    // TODO: Support other mask types.
    MaskReference(URL const& url)
        : m_url(url)
    {
    }

    URL const& url() const { return m_url; }

private:
    URL m_url;
};

// https://drafts.fxtf.org/css-masking/#the-clip-path
// TODO: Support clip sources.
class ClipPathReference {
public:
    ClipPathReference(URL const& url)
        : m_clip_source(url)
    {
    }

    ClipPathReference(BasicShapeStyleValue const& basic_shape)
        : m_clip_source(basic_shape)
    {
    }

    bool is_basic_shape() const { return m_clip_source.has<BasicShape>(); }

    bool is_url() const { return m_clip_source.has<URL>(); }

    URL const& url() const { return m_clip_source.get<URL>(); }

    BasicShapeStyleValue const& basic_shape() const { return *m_clip_source.get<BasicShape>(); }

private:
    using BasicShape = NonnullRefPtr<BasicShapeStyleValue const>;

    Variant<URL, BasicShape> m_clip_source;
};

struct BackgroundLayerData {
    RefPtr<AbstractImageStyleValue const> background_image { nullptr };
    BackgroundAttachment attachment { BackgroundAttachment::Scroll };
    BackgroundBox origin { BackgroundBox::PaddingBox };
    BackgroundBox clip { BackgroundBox::BorderBox };
    PositionEdge position_edge_x { PositionEdge::Left };
    LengthPercentage position_offset_x { Length::make_px(0) };
    PositionEdge position_edge_y { PositionEdge::Top };
    LengthPercentage position_offset_y { Length::make_px(0) };
    BackgroundSize size_type { BackgroundSize::LengthPercentage };
    LengthPercentageOrAuto size_x { LengthPercentageOrAuto::make_auto() };
    LengthPercentageOrAuto size_y { LengthPercentageOrAuto::make_auto() };
    Repetition repeat_x { Repetition::Repeat };
    Repetition repeat_y { Repetition::Repeat };
    MixBlendMode blend_mode { MixBlendMode::Normal };
};

struct BorderData {
public:
    Color color { Color::Transparent };
    LineStyle line_style { LineStyle::None };
    CSSPixels width { 0 };

    bool operator==(BorderData const&) const = default;
};

struct TouchActionData {
    bool allow_left : 1 { true };
    bool allow_right : 1 { true };
    bool allow_up : 1 { true };
    bool allow_down : 1 { true };
    bool allow_pinch_zoom : 1 { true };

    // Other touch interactions which aren't pan or pinch to zoom. E.g.: Double tap to zoom.
    bool allow_other : 1 { true };

    static TouchActionData none()
    {
        return TouchActionData {
            .allow_left = false,
            .allow_right = false,
            .allow_up = false,
            .allow_down = false,
            .allow_pinch_zoom = false,
            .allow_other = false,
        };
    }
};

struct WhiteSpaceTrimData {
    bool discard_before : 1 { false };
    bool discard_after : 1 { false };
    bool discard_inner : 1 { false };
};

struct TransformOrigin {
    LengthPercentage x { Percentage(50) };
    LengthPercentage y { Percentage(50) };
    LengthPercentage z { Percentage(0) };
};

struct ShadowData {
    Length offset_x { Length::make_px(0) };
    Length offset_y { Length::make_px(0) };
    Length blur_radius { Length::make_px(0) };
    Length spread_distance { Length::make_px(0) };
    Color color {};
    ShadowPlacement placement { ShadowPlacement::Outer };
};

struct ContentData {
    enum class Type {
        Normal,
        None,
        List,
    } type { Type::Normal };

    Vector<Variant<String, NonnullRefPtr<ImageStyleValue>>> data;
    Optional<String> alt_text {};
};

struct CounterData {
    FlyString name;
    bool is_reversed;
    Optional<CounterValue> value;
};

struct BorderRadiusData {
    LengthPercentage horizontal_radius { InitialValues::border_radius() };
    LengthPercentage vertical_radius { InitialValues::border_radius() };

    [[nodiscard]] bool is_initial() const
    {
        return horizontal_radius.is_length() && horizontal_radius.length().is_px() && horizontal_radius.length().absolute_length_to_px() == 0
            && vertical_radius.is_length() && vertical_radius.length().is_px() && vertical_radius.length().absolute_length_to_px() == 0;
    }
};

struct TextDecorationThickness {
    struct Auto { };
    struct FromFont { };
    Variant<Auto, FromFont, LengthPercentage> value;
};

// FIXME: Find a better place for this helper.
inline Gfx::ScalingMode to_gfx_scaling_mode(ImageRendering css_value, Gfx::IntRect source, Gfx::IntRect target)
{
    switch (css_value) {
    case ImageRendering::Auto:
    case ImageRendering::HighQuality:
    case ImageRendering::Smooth:
        if (target.width() < source.width() || target.height() < source.height())
            return Gfx::ScalingMode::BoxSampling;
        return Gfx::ScalingMode::BilinearBlend;
    case ImageRendering::CrispEdges:
        return Gfx::ScalingMode::NearestNeighbor;
    case ImageRendering::Pixelated:
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
    Float float_() const { return m_noninherited.float_; }
    Length border_spacing_horizontal() const { return m_inherited.border_spacing_horizontal; }
    Length border_spacing_vertical() const { return m_inherited.border_spacing_vertical; }
    CaptionSide caption_side() const { return m_inherited.caption_side; }
    Color caret_color() const { return m_inherited.caret_color; }
    Clear clear() const { return m_noninherited.clear; }
    Clip clip() const { return m_noninherited.clip; }
    ColorInterpolation color_interpolation() const { return m_inherited.color_interpolation; }
    PreferredColorScheme color_scheme() const { return m_inherited.color_scheme; }
    ContentVisibility content_visibility() const { return m_inherited.content_visibility; }
    Vector<CursorData> const& cursor() const { return m_inherited.cursor; }
    ContentData content() const { return m_noninherited.content; }
    PointerEvents pointer_events() const { return m_inherited.pointer_events; }
    Display display() const { return m_noninherited.display; }
    Display display_before_box_type_transformation() const { return m_noninherited.display_before_box_type_transformation; }
    Optional<int> const& z_index() const { return m_noninherited.z_index; }
    Variant<LengthOrCalculated, NumberOrCalculated> tab_size() const { return m_inherited.tab_size; }
    TextAlign text_align() const { return m_inherited.text_align; }
    TextJustify text_justify() const { return m_inherited.text_justify; }
    LengthPercentage const& text_indent() const { return m_inherited.text_indent; }
    TextWrapMode text_wrap_mode() const { return m_inherited.text_wrap_mode; }
    TextRendering text_rendering() const { return m_inherited.text_rendering; }
    CSSPixels text_underline_offset() const { return m_inherited.text_underline_offset; }
    TextUnderlinePosition text_underline_position() const { return m_inherited.text_underline_position; }
    Vector<TextDecorationLine> const& text_decoration_line() const { return m_noninherited.text_decoration_line; }
    TextDecorationThickness const& text_decoration_thickness() const { return m_noninherited.text_decoration_thickness; }
    TextDecorationStyle text_decoration_style() const { return m_noninherited.text_decoration_style; }
    Color text_decoration_color() const { return m_noninherited.text_decoration_color; }
    TextTransform text_transform() const { return m_inherited.text_transform; }
    TextOverflow text_overflow() const { return m_noninherited.text_overflow; }
    Vector<ShadowData> const& text_shadow() const { return m_inherited.text_shadow; }
    Positioning position() const { return m_noninherited.position; }
    WhiteSpaceCollapse white_space_collapse() const { return m_inherited.white_space_collapse; }
    WhiteSpaceTrimData white_space_trim() const { return m_noninherited.white_space_trim; }
    CSSPixels const& word_spacing() const { return m_inherited.word_spacing; }
    CSSPixels letter_spacing() const { return m_inherited.letter_spacing; }
    FlexDirection flex_direction() const { return m_noninherited.flex_direction; }
    FlexWrap flex_wrap() const { return m_noninherited.flex_wrap; }
    FlexBasis const& flex_basis() const { return m_noninherited.flex_basis; }
    float flex_grow() const { return m_noninherited.flex_grow; }
    float flex_shrink() const { return m_noninherited.flex_shrink; }
    int order() const { return m_noninherited.order; }
    Optional<Color> accent_color() const { return m_inherited.accent_color; }
    AlignContent align_content() const { return m_noninherited.align_content; }
    AlignItems align_items() const { return m_noninherited.align_items; }
    AlignSelf align_self() const { return m_noninherited.align_self; }
    Appearance appearance() const { return m_noninherited.appearance; }
    float opacity() const { return m_noninherited.opacity; }
    Visibility visibility() const { return m_inherited.visibility; }
    ImageRendering image_rendering() const { return m_inherited.image_rendering; }
    JustifyContent justify_content() const { return m_noninherited.justify_content; }
    JustifySelf justify_self() const { return m_noninherited.justify_self; }
    JustifyItems justify_items() const { return m_noninherited.justify_items; }
    Filter const& backdrop_filter() const { return m_noninherited.backdrop_filter; }
    Filter const& filter() const { return m_noninherited.filter; }
    Vector<ShadowData> const& box_shadow() const { return m_noninherited.box_shadow; }
    BoxSizing box_sizing() const { return m_noninherited.box_sizing; }
    Size const& width() const { return m_noninherited.width; }
    Size const& min_width() const { return m_noninherited.min_width; }
    Size const& max_width() const { return m_noninherited.max_width; }
    Size const& height() const { return m_noninherited.height; }
    Size const& min_height() const { return m_noninherited.min_height; }
    Size const& max_height() const { return m_noninherited.max_height; }
    Variant<VerticalAlign, LengthPercentage> const& vertical_align() const { return m_noninherited.vertical_align; }
    GridTrackSizeList const& grid_auto_columns() const { return m_noninherited.grid_auto_columns; }
    GridTrackSizeList const& grid_auto_rows() const { return m_noninherited.grid_auto_rows; }
    GridAutoFlow const& grid_auto_flow() const { return m_noninherited.grid_auto_flow; }
    GridTrackSizeList const& grid_template_columns() const { return m_noninherited.grid_template_columns; }
    GridTrackSizeList const& grid_template_rows() const { return m_noninherited.grid_template_rows; }
    GridTrackPlacement const& grid_column_end() const { return m_noninherited.grid_column_end; }
    GridTrackPlacement const& grid_column_start() const { return m_noninherited.grid_column_start; }
    GridTrackPlacement const& grid_row_end() const { return m_noninherited.grid_row_end; }
    GridTrackPlacement const& grid_row_start() const { return m_noninherited.grid_row_start; }
    ColumnCount column_count() const { return m_noninherited.column_count; }
    Variant<LengthPercentage, NormalGap> const& column_gap() const { return m_noninherited.column_gap; }
    ColumnSpan const& column_span() const { return m_noninherited.column_span; }
    Size const& column_width() const { return m_noninherited.column_width; }
    Size const& column_height() const { return m_noninherited.column_height; }
    Variant<LengthPercentage, NormalGap> const& row_gap() const { return m_noninherited.row_gap; }
    BorderCollapse border_collapse() const { return m_inherited.border_collapse; }
    EmptyCells empty_cells() const { return m_inherited.empty_cells; }
    Vector<Vector<String>> const& grid_template_areas() const { return m_noninherited.grid_template_areas; }
    ObjectFit object_fit() const { return m_noninherited.object_fit; }
    ObjectPosition object_position() const { return m_noninherited.object_position; }
    Direction direction() const { return m_inherited.direction; }
    UnicodeBidi unicode_bidi() const { return m_noninherited.unicode_bidi; }
    WritingMode writing_mode() const { return m_inherited.writing_mode; }
    UserSelect user_select() const { return m_noninherited.user_select; }
    Isolation isolation() const { return m_noninherited.isolation; }
    Containment const& contain() const { return m_noninherited.contain; }
    ContainerType const& container_type() const { return m_noninherited.container_type; }
    MixBlendMode mix_blend_mode() const { return m_noninherited.mix_blend_mode; }
    Optional<FlyString> view_transition_name() const { return m_noninherited.view_transition_name; }
    TouchActionData touch_action() const { return m_noninherited.touch_action; }
    ShapeRendering shape_rendering() const { return m_noninherited.shape_rendering; }

    LengthBox const& inset() const { return m_noninherited.inset; }
    LengthBox const& margin() const { return m_noninherited.margin; }
    LengthBox const& padding() const { return m_noninherited.padding; }

    BorderData const& border_left() const { return m_noninherited.border_left; }
    BorderData const& border_top() const { return m_noninherited.border_top; }
    BorderData const& border_right() const { return m_noninherited.border_right; }
    BorderData const& border_bottom() const { return m_noninherited.border_bottom; }

    bool has_noninitial_border_radii() const { return m_noninherited.has_noninitial_border_radii; }
    BorderRadiusData const& border_bottom_left_radius() const { return m_noninherited.border_bottom_left_radius; }
    BorderRadiusData const& border_bottom_right_radius() const { return m_noninherited.border_bottom_right_radius; }
    BorderRadiusData const& border_top_left_radius() const { return m_noninherited.border_top_left_radius; }
    BorderRadiusData const& border_top_right_radius() const { return m_noninherited.border_top_right_radius; }

    Overflow overflow_x() const { return m_noninherited.overflow_x; }
    Overflow overflow_y() const { return m_noninherited.overflow_y; }

    Color color() const { return m_inherited.color; }
    Color background_color() const { return m_noninherited.background_color; }
    Vector<BackgroundLayerData> const& background_layers() const { return m_noninherited.background_layers; }

    Color webkit_text_fill_color() const { return m_inherited.webkit_text_fill_color; }

    ListStyleType list_style_type() const { return m_inherited.list_style_type; }
    ListStylePosition list_style_position() const { return m_inherited.list_style_position; }

    Optional<SVGPaint> const& fill() const { return m_inherited.fill; }
    FillRule fill_rule() const { return m_inherited.fill_rule; }
    Optional<SVGPaint> const& stroke() const { return m_inherited.stroke; }
    float fill_opacity() const { return m_inherited.fill_opacity; }
    Vector<Variant<LengthPercentage, NumberOrCalculated>> const& stroke_dasharray() const { return m_inherited.stroke_dasharray; }
    LengthPercentage const& stroke_dashoffset() const { return m_inherited.stroke_dashoffset; }
    StrokeLinecap stroke_linecap() const { return m_inherited.stroke_linecap; }
    StrokeLinejoin stroke_linejoin() const { return m_inherited.stroke_linejoin; }
    NumberOrCalculated stroke_miterlimit() const { return m_inherited.stroke_miterlimit; }
    float stroke_opacity() const { return m_inherited.stroke_opacity; }
    LengthPercentage const& stroke_width() const { return m_inherited.stroke_width; }
    Color stop_color() const { return m_noninherited.stop_color; }
    float stop_opacity() const { return m_noninherited.stop_opacity; }
    TextAnchor text_anchor() const { return m_inherited.text_anchor; }
    RefPtr<AbstractImageStyleValue const> mask_image() const { return m_noninherited.mask_image; }
    Optional<MaskReference> const& mask() const { return m_noninherited.mask; }
    MaskType mask_type() const { return m_noninherited.mask_type; }
    Optional<ClipPathReference> const& clip_path() const { return m_noninherited.clip_path; }
    ClipRule clip_rule() const { return m_inherited.clip_rule; }
    Color flood_color() const { return m_noninherited.flood_color; }
    float flood_opacity() const { return m_noninherited.flood_opacity; }
    PaintOrderList paint_order() const { return m_inherited.paint_order; }

    LengthPercentage const& cx() const { return m_noninherited.cx; }
    LengthPercentage const& cy() const { return m_noninherited.cy; }
    LengthPercentage const& r() const { return m_noninherited.r; }
    LengthPercentageOrAuto const& rx() const { return m_noninherited.ry; }
    LengthPercentageOrAuto const& ry() const { return m_noninherited.ry; }
    LengthPercentage const& x() const { return m_noninherited.x; }
    LengthPercentage const& y() const { return m_noninherited.y; }

    Vector<Transformation> const& transformations() const { return m_noninherited.transformations; }
    TransformBox const& transform_box() const { return m_noninherited.transform_box; }
    TransformOrigin const& transform_origin() const { return m_noninherited.transform_origin; }
    Optional<Transformation> const& rotate() const { return m_noninherited.rotate; }
    Optional<Transformation> const& translate() const { return m_noninherited.translate; }
    Optional<Transformation> const& scale() const { return m_noninherited.scale; }

    Gfx::FontCascadeList const& font_list() const { return *m_inherited.font_list; }
    CSSPixels font_size() const { return m_inherited.font_size; }
    double font_weight() const { return m_inherited.font_weight; }
    Optional<Gfx::FontVariantAlternates> font_variant_alternates() const { return m_inherited.font_variant_alternates; }
    FontVariantCaps font_variant_caps() const { return m_inherited.font_variant_caps; }
    Optional<Gfx::FontVariantEastAsian> font_variant_east_asian() const { return m_inherited.font_variant_east_asian; }
    FontVariantEmoji font_variant_emoji() const { return m_inherited.font_variant_emoji; }
    Optional<Gfx::FontVariantLigatures> font_variant_ligatures() const { return m_inherited.font_variant_ligatures; }
    Optional<Gfx::FontVariantNumeric> font_variant_numeric() const { return m_inherited.font_variant_numeric; }
    FontVariantPosition font_variant_position() const { return m_inherited.font_variant_position; }
    FontKerning font_kerning() const { return m_inherited.font_kerning; }
    Optional<FlyString> font_language_override() const { return m_inherited.font_language_override; }
    Optional<HashMap<FlyString, IntegerOrCalculated>> font_feature_settings() const { return m_inherited.font_feature_settings; }
    Optional<HashMap<FlyString, NumberOrCalculated>> font_variation_settings() const { return m_inherited.font_variation_settings; }
    CSSPixels line_height() const { return m_inherited.line_height; }
    Time transition_delay() const { return m_noninherited.transition_delay; }

    Color outline_color() const { return m_noninherited.outline_color; }
    Length outline_offset() const { return m_noninherited.outline_offset; }
    OutlineStyle outline_style() const { return m_noninherited.outline_style; }
    CSSPixels outline_width() const { return m_noninherited.outline_width; }

    TableLayout table_layout() const { return m_noninherited.table_layout; }

    QuotesData quotes() const { return m_inherited.quotes; }

    MathShift math_shift() const { return m_inherited.math_shift; }
    MathStyle math_style() const { return m_inherited.math_style; }
    int math_depth() const { return m_inherited.math_depth; }

    ScrollbarColorData scrollbar_color() const { return m_inherited.scrollbar_color; }
    ScrollbarWidth scrollbar_width() const { return m_noninherited.scrollbar_width; }

    WillChange will_change() const { return m_noninherited.will_change; }

    NonnullOwnPtr<ComputedValues> clone_inherited_values() const
    {
        auto clone = make<ComputedValues>();
        clone->m_inherited = m_inherited;
        return clone;
    }

protected:
    struct {
        Color caret_color { InitialValues::caret_color() };
        RefPtr<Gfx::FontCascadeList const> font_list {};
        CSSPixels font_size { InitialValues::font_size() };
        double font_weight { InitialValues::font_weight() };
        Optional<Gfx::FontVariantAlternates> font_variant_alternates;
        FontVariantCaps font_variant_caps { FontVariantCaps::Normal };
        Optional<Gfx::FontVariantEastAsian> font_variant_east_asian;
        FontVariantEmoji font_variant_emoji { FontVariantEmoji::Normal };
        Optional<Gfx::FontVariantLigatures> font_variant_ligatures;
        Optional<Gfx::FontVariantNumeric> font_variant_numeric;
        FontVariantPosition font_variant_position { FontVariantPosition::Normal };
        FontKerning font_kerning { InitialValues::font_kerning() };
        Optional<FlyString> font_language_override;
        Optional<HashMap<FlyString, IntegerOrCalculated>> font_feature_settings;
        Optional<HashMap<FlyString, NumberOrCalculated>> font_variation_settings;
        CSSPixels line_height { InitialValues::line_height() };
        BorderCollapse border_collapse { InitialValues::border_collapse() };
        EmptyCells empty_cells { InitialValues::empty_cells() };
        Length border_spacing_horizontal { InitialValues::border_spacing() };
        Length border_spacing_vertical { InitialValues::border_spacing() };
        CaptionSide caption_side { InitialValues::caption_side() };
        Color color { InitialValues::color() };
        ColorInterpolation color_interpolation { InitialValues::color_interpolation() };
        PreferredColorScheme color_scheme { InitialValues::color_scheme() };
        Optional<Color> accent_color {};
        Color webkit_text_fill_color { InitialValues::color() };
        ContentVisibility content_visibility { InitialValues::content_visibility() };
        Vector<CursorData> cursor { InitialValues::cursor() };
        ImageRendering image_rendering { InitialValues::image_rendering() };
        PointerEvents pointer_events { InitialValues::pointer_events() };
        Variant<LengthOrCalculated, NumberOrCalculated> tab_size { InitialValues::tab_size() };
        TextAlign text_align { InitialValues::text_align() };
        TextJustify text_justify { InitialValues::text_justify() };
        TextTransform text_transform { InitialValues::text_transform() };
        LengthPercentage text_indent { InitialValues::text_indent() };
        TextWrapMode text_wrap_mode { InitialValues::text_wrap_mode() };
        TextRendering text_rendering { InitialValues::text_rendering() };
        CSSPixels text_underline_offset { InitialValues::text_underline_offset() };
        TextUnderlinePosition text_underline_position { InitialValues::text_underline_position() };
        WhiteSpaceCollapse white_space_collapse { InitialValues::white_space_collapse() };
        WordBreak word_break { InitialValues::word_break() };
        CSSPixels word_spacing { InitialValues::word_spacing() };
        CSSPixels letter_spacing { InitialValues::letter_spacing() };
        ListStyleType list_style_type { InitialValues::list_style_type() };
        ListStylePosition list_style_position { InitialValues::list_style_position() };
        Visibility visibility { InitialValues::visibility() };
        QuotesData quotes { InitialValues::quotes() };
        Direction direction { InitialValues::direction() };
        WritingMode writing_mode { InitialValues::writing_mode() };

        Optional<SVGPaint> fill;
        FillRule fill_rule { InitialValues::fill_rule() };
        Optional<SVGPaint> stroke;
        float fill_opacity { InitialValues::fill_opacity() };
        PaintOrderList paint_order { InitialValues::paint_order() };
        Vector<Variant<LengthPercentage, NumberOrCalculated>> stroke_dasharray;
        LengthPercentage stroke_dashoffset { InitialValues::stroke_dashoffset() };
        StrokeLinecap stroke_linecap { InitialValues::stroke_linecap() };
        StrokeLinejoin stroke_linejoin { InitialValues::stroke_linejoin() };
        NumberOrCalculated stroke_miterlimit { InitialValues::stroke_miterlimit() };
        float stroke_opacity { InitialValues::stroke_opacity() };
        LengthPercentage stroke_width { InitialValues::stroke_width() };
        TextAnchor text_anchor { InitialValues::text_anchor() };
        ClipRule clip_rule { InitialValues::clip_rule() };

        Vector<ShadowData> text_shadow;

        MathShift math_shift { InitialValues::math_shift() };
        MathStyle math_style { InitialValues::math_style() };
        int math_depth { InitialValues::math_depth() };

        ScrollbarColorData scrollbar_color { InitialValues::scrollbar_color() };
    } m_inherited;

    struct {
        AspectRatio aspect_ratio { InitialValues::aspect_ratio() };
        Float float_ { InitialValues::float_() };
        Clear clear { InitialValues::clear() };
        Clip clip { InitialValues::clip() };
        Display display { InitialValues::display() };
        Display display_before_box_type_transformation { InitialValues::display() };
        Optional<int> z_index;
        // FIXME: Store this as flags in a u8.
        Vector<TextDecorationLine> text_decoration_line { InitialValues::text_decoration_line() };
        TextDecorationThickness text_decoration_thickness { TextDecorationThickness::Auto {} };
        TextDecorationStyle text_decoration_style { InitialValues::text_decoration_style() };
        Color text_decoration_color { InitialValues::color() };
        TextOverflow text_overflow { InitialValues::text_overflow() };
        Positioning position { InitialValues::position() };
        Size width { InitialValues::width() };
        Size min_width { InitialValues::min_width() };
        Size max_width { InitialValues::max_width() };
        Size height { InitialValues::height() };
        Size min_height { InitialValues::min_height() };
        Size max_height { InitialValues::max_height() };
        LengthBox inset { InitialValues::inset() };
        LengthBox margin { InitialValues::margin() };
        LengthBox padding { InitialValues::padding() };
        Filter backdrop_filter { InitialValues::backdrop_filter() };
        Filter filter { InitialValues::filter() };
        BorderData border_left;
        BorderData border_top;
        BorderData border_right;
        BorderData border_bottom;
        bool has_noninitial_border_radii { false };
        BorderRadiusData border_bottom_left_radius;
        BorderRadiusData border_bottom_right_radius;
        BorderRadiusData border_top_left_radius;
        BorderRadiusData border_top_right_radius;
        Color background_color { InitialValues::background_color() };
        Vector<BackgroundLayerData> background_layers;
        FlexDirection flex_direction { InitialValues::flex_direction() };
        FlexWrap flex_wrap { InitialValues::flex_wrap() };
        FlexBasis flex_basis { InitialValues::flex_basis() };
        float flex_grow { InitialValues::flex_grow() };
        float flex_shrink { InitialValues::flex_shrink() };
        int order { InitialValues::order() };
        AlignContent align_content { InitialValues::align_content() };
        AlignItems align_items { InitialValues::align_items() };
        AlignSelf align_self { InitialValues::align_self() };
        Appearance appearance { InitialValues::appearance() };
        JustifyContent justify_content { InitialValues::justify_content() };
        JustifyItems justify_items { InitialValues::justify_items() };
        JustifySelf justify_self { InitialValues::justify_self() };
        Overflow overflow_x { InitialValues::overflow() };
        Overflow overflow_y { InitialValues::overflow() };
        float opacity { InitialValues::opacity() };
        Vector<ShadowData> box_shadow {};
        Vector<Transformation> transformations {};
        TransformBox transform_box { InitialValues::transform_box() };
        TransformOrigin transform_origin {};
        BoxSizing box_sizing { InitialValues::box_sizing() };
        ContentData content;
        Variant<VerticalAlign, LengthPercentage> vertical_align { InitialValues::vertical_align() };
        GridTrackSizeList grid_auto_columns;
        GridTrackSizeList grid_auto_rows;
        GridTrackSizeList grid_template_columns;
        GridTrackSizeList grid_template_rows;
        GridAutoFlow grid_auto_flow { InitialValues::grid_auto_flow() };
        GridTrackPlacement grid_column_end { InitialValues::grid_column_end() };
        GridTrackPlacement grid_column_start { InitialValues::grid_column_start() };
        GridTrackPlacement grid_row_end { InitialValues::grid_row_end() };
        GridTrackPlacement grid_row_start { InitialValues::grid_row_start() };
        ColumnCount column_count { InitialValues::column_count() };
        Variant<LengthPercentage, NormalGap> column_gap { InitialValues::column_gap() };
        ColumnSpan column_span { InitialValues::column_span() };
        Size column_width { InitialValues::column_width() };
        Size column_height { InitialValues::column_height() };
        Variant<LengthPercentage, NormalGap> row_gap { InitialValues::row_gap() };
        Vector<Vector<String>> grid_template_areas { InitialValues::grid_template_areas() };
        Gfx::Color stop_color { InitialValues::stop_color() };
        float stop_opacity { InitialValues::stop_opacity() };
        Time transition_delay { InitialValues::transition_delay() };
        Color outline_color { InitialValues::outline_color() };
        Length outline_offset { InitialValues::outline_offset() };
        OutlineStyle outline_style { InitialValues::outline_style() };
        CSSPixels outline_width { InitialValues::outline_width() };
        TableLayout table_layout { InitialValues::table_layout() };
        ObjectFit object_fit { InitialValues::object_fit() };
        ObjectPosition object_position { InitialValues::object_position() };
        UnicodeBidi unicode_bidi { InitialValues::unicode_bidi() };
        UserSelect user_select { InitialValues::user_select() };
        Isolation isolation { InitialValues::isolation() };
        Containment contain { InitialValues::contain() };
        ContainerType container_type { InitialValues::container_type() };
        MixBlendMode mix_blend_mode { InitialValues::mix_blend_mode() };
        WhiteSpaceTrimData white_space_trim;
        Optional<FlyString> view_transition_name;
        TouchActionData touch_action;

        Optional<Transformation> rotate;
        Optional<Transformation> translate;
        Optional<Transformation> scale;

        Optional<MaskReference> mask;
        MaskType mask_type { InitialValues::mask_type() };
        Optional<ClipPathReference> clip_path;
        RefPtr<AbstractImageStyleValue const> mask_image;

        LengthPercentage cx { InitialValues::cx() };
        LengthPercentage cy { InitialValues::cy() };
        LengthPercentage r { InitialValues::r() };
        LengthPercentageOrAuto rx { InitialValues::rx() };
        LengthPercentageOrAuto ry { InitialValues::ry() };
        LengthPercentage x { InitialValues::x() };
        LengthPercentage y { InitialValues::x() };

        ScrollbarWidth scrollbar_width { InitialValues::scrollbar_width() };
        Vector<CounterData, 0> counter_increment;
        Vector<CounterData, 0> counter_reset;
        Vector<CounterData, 0> counter_set;

        WillChange will_change { InitialValues::will_change() };

        Color flood_color { InitialValues::flood_color() };
        float flood_opacity { InitialValues::flood_opacity() };

        ShapeRendering shape_rendering { InitialValues::shape_rendering() };
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

    void set_aspect_ratio(AspectRatio aspect_ratio) { m_noninherited.aspect_ratio = move(aspect_ratio); }
    void set_caret_color(Color caret_color) { m_inherited.caret_color = caret_color; }
    void set_font_list(NonnullRefPtr<Gfx::FontCascadeList const> font_list) { m_inherited.font_list = move(font_list); }
    void set_font_size(CSSPixels font_size) { m_inherited.font_size = font_size; }
    void set_font_weight(double font_weight) { m_inherited.font_weight = font_weight; }
    void set_font_variant_alternates(Optional<Gfx::FontVariantAlternates> font_variant_alternates) { m_inherited.font_variant_alternates = move(font_variant_alternates); }
    void set_font_variant_caps(FontVariantCaps font_variant_caps) { m_inherited.font_variant_caps = font_variant_caps; }
    void set_font_variant_east_asian(Optional<Gfx::FontVariantEastAsian> font_variant_east_asian) { m_inherited.font_variant_east_asian = move(font_variant_east_asian); }
    void set_font_variant_emoji(FontVariantEmoji font_variant_emoji) { m_inherited.font_variant_emoji = font_variant_emoji; }
    void set_font_variant_ligatures(Optional<Gfx::FontVariantLigatures> font_variant_ligatures) { m_inherited.font_variant_ligatures = move(font_variant_ligatures); }
    void set_font_variant_numeric(Optional<Gfx::FontVariantNumeric> font_variant_numeric) { m_inherited.font_variant_numeric = move(font_variant_numeric); }
    void set_font_variant_position(FontVariantPosition font_variant_position) { m_inherited.font_variant_position = font_variant_position; }
    void set_font_kerning(FontKerning font_kerning) { m_inherited.font_kerning = font_kerning; }
    void set_font_language_override(Optional<FlyString> font_language_override) { m_inherited.font_language_override = move(font_language_override); }
    void set_font_feature_settings(Optional<HashMap<FlyString, IntegerOrCalculated>> value) { m_inherited.font_feature_settings = move(value); }
    void set_font_variation_settings(Optional<HashMap<FlyString, NumberOrCalculated>> value) { m_inherited.font_variation_settings = move(value); }
    void set_line_height(CSSPixels line_height) { m_inherited.line_height = line_height; }
    void set_border_spacing_horizontal(Length border_spacing_horizontal) { m_inherited.border_spacing_horizontal = move(border_spacing_horizontal); }
    void set_border_spacing_vertical(Length border_spacing_vertical) { m_inherited.border_spacing_vertical = move(border_spacing_vertical); }
    void set_caption_side(CaptionSide caption_side) { m_inherited.caption_side = caption_side; }
    void set_color(Color color) { m_inherited.color = color; }
    void set_color_interpolation(ColorInterpolation color_interpolation) { m_inherited.color_interpolation = color_interpolation; }
    void set_color_scheme(PreferredColorScheme color_scheme) { m_inherited.color_scheme = color_scheme; }
    void set_clip(Clip const& clip) { m_noninherited.clip = clip; }
    void set_content(ContentData const& content) { m_noninherited.content = content; }
    void set_content_visibility(ContentVisibility content_visibility) { m_inherited.content_visibility = content_visibility; }
    void set_cursor(Vector<CursorData> cursor) { m_inherited.cursor = move(cursor); }
    void set_image_rendering(ImageRendering value) { m_inherited.image_rendering = value; }
    void set_pointer_events(PointerEvents value) { m_inherited.pointer_events = value; }
    void set_background_color(Color color) { m_noninherited.background_color = color; }
    void set_background_layers(Vector<BackgroundLayerData>&& layers) { m_noninherited.background_layers = move(layers); }
    void set_float(Float value) { m_noninherited.float_ = value; }
    void set_clear(Clear value) { m_noninherited.clear = value; }
    void set_z_index(Optional<int> value) { m_noninherited.z_index = move(value); }
    void set_tab_size(Variant<LengthOrCalculated, NumberOrCalculated> value) { m_inherited.tab_size = move(value); }
    void set_text_align(TextAlign text_align) { m_inherited.text_align = text_align; }
    void set_text_justify(TextJustify text_justify) { m_inherited.text_justify = text_justify; }
    void set_text_decoration_line(Vector<TextDecorationLine> value) { m_noninherited.text_decoration_line = move(value); }
    void set_text_decoration_thickness(TextDecorationThickness value) { m_noninherited.text_decoration_thickness = move(value); }
    void set_text_decoration_style(TextDecorationStyle value) { m_noninherited.text_decoration_style = value; }
    void set_text_decoration_color(Color value) { m_noninherited.text_decoration_color = value; }
    void set_text_transform(TextTransform value) { m_inherited.text_transform = value; }
    void set_text_shadow(Vector<ShadowData>&& value) { m_inherited.text_shadow = move(value); }
    void set_text_indent(LengthPercentage value) { m_inherited.text_indent = move(value); }
    void set_text_wrap_mode(TextWrapMode value) { m_inherited.text_wrap_mode = value; }
    void set_text_overflow(TextOverflow value) { m_noninherited.text_overflow = value; }
    void set_text_rendering(TextRendering value) { m_inherited.text_rendering = value; }
    void set_text_underline_offset(CSSPixels value) { m_inherited.text_underline_offset = value; }
    void set_text_underline_position(TextUnderlinePosition value) { m_inherited.text_underline_position = value; }
    void set_webkit_text_fill_color(Color value) { m_inherited.webkit_text_fill_color = value; }
    void set_position(Positioning position) { m_noninherited.position = position; }
    void set_white_space_collapse(WhiteSpaceCollapse value) { m_inherited.white_space_collapse = value; }
    void set_white_space_trim(WhiteSpaceTrimData value) { m_noninherited.white_space_trim = value; }
    void set_word_spacing(CSSPixels value) { m_inherited.word_spacing = value; }
    void set_word_break(WordBreak value) { m_inherited.word_break = value; }
    void set_letter_spacing(CSSPixels value) { m_inherited.letter_spacing = value; }
    void set_width(Size const& width) { m_noninherited.width = width; }
    void set_min_width(Size const& width) { m_noninherited.min_width = width; }
    void set_max_width(Size const& width) { m_noninherited.max_width = width; }
    void set_height(Size const& height) { m_noninherited.height = height; }
    void set_min_height(Size const& height) { m_noninherited.min_height = height; }
    void set_max_height(Size const& height) { m_noninherited.max_height = height; }
    void set_inset(LengthBox const& inset) { m_noninherited.inset = inset; }
    void set_margin(LengthBox const& margin) { m_noninherited.margin = margin; }
    void set_padding(LengthBox const& padding) { m_noninherited.padding = padding; }
    void set_overflow_x(Overflow value) { m_noninherited.overflow_x = value; }
    void set_overflow_y(Overflow value) { m_noninherited.overflow_y = value; }
    void set_list_style_type(ListStyleType value) { m_inherited.list_style_type = move(value); }
    void set_list_style_position(ListStylePosition value) { m_inherited.list_style_position = move(value); }
    void set_display(Display value) { m_noninherited.display = value; }
    void set_display_before_box_type_transformation(Display value) { m_noninherited.display_before_box_type_transformation = value; }
    void set_backdrop_filter(Filter const& backdrop_filter) { m_noninherited.backdrop_filter = backdrop_filter; }
    void set_filter(Filter const& filter) { m_noninherited.filter = filter; }
    void set_border_bottom_left_radius(BorderRadiusData value)
    {
        if (value.is_initial() && !m_noninherited.has_noninitial_border_radii)
            return;
        m_noninherited.has_noninitial_border_radii = true;
        m_noninherited.border_bottom_left_radius = move(value);
    }
    void set_border_bottom_right_radius(BorderRadiusData value)
    {
        if (value.is_initial() && !m_noninherited.has_noninitial_border_radii)
            return;
        m_noninherited.has_noninitial_border_radii = true;
        m_noninherited.border_bottom_right_radius = move(value);
    }
    void set_border_top_left_radius(BorderRadiusData value)
    {
        if (value.is_initial() && !m_noninherited.has_noninitial_border_radii)
            return;
        m_noninherited.has_noninitial_border_radii = true;
        m_noninherited.border_top_left_radius = move(value);
    }
    void set_border_top_right_radius(BorderRadiusData value)
    {
        if (value.is_initial() && !m_noninherited.has_noninitial_border_radii)
            return;
        m_noninherited.has_noninitial_border_radii = true;
        m_noninherited.border_top_right_radius = move(value);
    }
    BorderData& border_left() { return m_noninherited.border_left; }
    BorderData& border_top() { return m_noninherited.border_top; }
    BorderData& border_right() { return m_noninherited.border_right; }
    BorderData& border_bottom() { return m_noninherited.border_bottom; }
    void set_flex_direction(FlexDirection value) { m_noninherited.flex_direction = value; }
    void set_flex_wrap(FlexWrap value) { m_noninherited.flex_wrap = value; }
    void set_flex_basis(FlexBasis value) { m_noninherited.flex_basis = move(value); }
    void set_flex_grow(float value) { m_noninherited.flex_grow = value; }
    void set_flex_shrink(float value) { m_noninherited.flex_shrink = value; }
    void set_order(int value) { m_noninherited.order = value; }
    void set_accent_color(Color value) { m_inherited.accent_color = value; }
    void set_align_content(AlignContent value) { m_noninherited.align_content = value; }
    void set_align_items(AlignItems value) { m_noninherited.align_items = value; }
    void set_align_self(AlignSelf value) { m_noninherited.align_self = value; }
    void set_appearance(Appearance value) { m_noninherited.appearance = value; }
    void set_opacity(float value) { m_noninherited.opacity = value; }
    void set_justify_content(JustifyContent value) { m_noninherited.justify_content = value; }
    void set_justify_items(JustifyItems value) { m_noninherited.justify_items = value; }
    void set_justify_self(JustifySelf value) { m_noninherited.justify_self = value; }
    void set_box_shadow(Vector<ShadowData>&& value) { m_noninherited.box_shadow = move(value); }
    void set_rotate(Transformation value) { m_noninherited.rotate = move(value); }
    void set_scale(Transformation value) { m_noninherited.scale = move(value); }
    void set_transformations(Vector<Transformation> value) { m_noninherited.transformations = move(value); }
    void set_transform_box(TransformBox value) { m_noninherited.transform_box = value; }
    void set_transform_origin(TransformOrigin value) { m_noninherited.transform_origin = move(value); }
    void set_translate(Transformation value) { m_noninherited.translate = move(value); }
    void set_box_sizing(BoxSizing value) { m_noninherited.box_sizing = value; }
    void set_vertical_align(Variant<VerticalAlign, LengthPercentage> value) { m_noninherited.vertical_align = move(value); }
    void set_visibility(Visibility value) { m_inherited.visibility = value; }
    void set_grid_auto_columns(GridTrackSizeList value) { m_noninherited.grid_auto_columns = move(value); }
    void set_grid_auto_rows(GridTrackSizeList value) { m_noninherited.grid_auto_rows = move(value); }
    void set_grid_template_columns(GridTrackSizeList value) { m_noninherited.grid_template_columns = move(value); }
    void set_grid_template_rows(GridTrackSizeList value) { m_noninherited.grid_template_rows = move(value); }
    void set_grid_column_end(GridTrackPlacement value) { m_noninherited.grid_column_end = move(value); }
    void set_grid_column_start(GridTrackPlacement value) { m_noninherited.grid_column_start = move(value); }
    void set_grid_row_end(GridTrackPlacement value) { m_noninherited.grid_row_end = move(value); }
    void set_grid_row_start(GridTrackPlacement value) { m_noninherited.grid_row_start = move(value); }
    void set_column_count(ColumnCount value) { m_noninherited.column_count = value; }
    void set_column_gap(Variant<LengthPercentage, NormalGap> const& column_gap) { m_noninherited.column_gap = column_gap; }
    void set_column_span(ColumnSpan const column_span) { m_noninherited.column_span = column_span; }
    void set_column_width(Size const& column_width) { m_noninherited.column_width = column_width; }
    void set_column_height(Size const& column_height) { m_noninherited.column_height = column_height; }
    void set_row_gap(Variant<LengthPercentage, NormalGap> const& row_gap) { m_noninherited.row_gap = row_gap; }
    void set_border_collapse(BorderCollapse const border_collapse) { m_inherited.border_collapse = border_collapse; }
    void set_empty_cells(EmptyCells const empty_cells) { m_inherited.empty_cells = empty_cells; }
    void set_grid_template_areas(Vector<Vector<String>> const& grid_template_areas) { m_noninherited.grid_template_areas = grid_template_areas; }
    void set_grid_auto_flow(GridAutoFlow grid_auto_flow) { m_noninherited.grid_auto_flow = grid_auto_flow; }
    void set_transition_delay(Time const& transition_delay) { m_noninherited.transition_delay = transition_delay; }
    void set_table_layout(TableLayout value) { m_noninherited.table_layout = value; }
    void set_quotes(QuotesData value) { m_inherited.quotes = move(value); }
    void set_object_fit(ObjectFit value) { m_noninherited.object_fit = value; }
    void set_object_position(ObjectPosition value) { m_noninherited.object_position = move(value); }
    void set_direction(Direction value) { m_inherited.direction = value; }
    void set_unicode_bidi(UnicodeBidi value) { m_noninherited.unicode_bidi = value; }
    void set_writing_mode(WritingMode value) { m_inherited.writing_mode = value; }
    void set_user_select(UserSelect value) { m_noninherited.user_select = value; }
    void set_isolation(Isolation value) { m_noninherited.isolation = value; }
    void set_contain(Containment value) { m_noninherited.contain = move(value); }
    void set_container_type(ContainerType value) { m_noninherited.container_type = move(value); }
    void set_mix_blend_mode(MixBlendMode value) { m_noninherited.mix_blend_mode = value; }
    void set_view_transition_name(Optional<FlyString> value) { m_noninherited.view_transition_name = move(value); }
    void set_touch_action(TouchActionData value) { m_noninherited.touch_action = value; }

    void set_fill(SVGPaint value) { m_inherited.fill = move(value); }
    void set_stroke(SVGPaint value) { m_inherited.stroke = move(value); }
    void set_fill_rule(FillRule value) { m_inherited.fill_rule = value; }
    void set_fill_opacity(float value) { m_inherited.fill_opacity = value; }
    void set_stroke_dasharray(Vector<Variant<LengthPercentage, NumberOrCalculated>> value) { m_inherited.stroke_dasharray = move(value); }
    void set_stroke_dashoffset(LengthPercentage value) { m_inherited.stroke_dashoffset = move(value); }
    void set_stroke_linecap(StrokeLinecap value) { m_inherited.stroke_linecap = move(value); }
    void set_stroke_linejoin(StrokeLinejoin value) { m_inherited.stroke_linejoin = move(value); }
    void set_stroke_miterlimit(NumberOrCalculated value) { m_inherited.stroke_miterlimit = move(value); }
    void set_stroke_opacity(float value) { m_inherited.stroke_opacity = value; }
    void set_stroke_width(LengthPercentage value) { m_inherited.stroke_width = move(value); }
    void set_stop_color(Color value) { m_noninherited.stop_color = value; }
    void set_stop_opacity(float value) { m_noninherited.stop_opacity = value; }
    void set_text_anchor(TextAnchor value) { m_inherited.text_anchor = value; }
    void set_outline_color(Color value) { m_noninherited.outline_color = value; }
    void set_outline_offset(Length value) { m_noninherited.outline_offset = move(value); }
    void set_outline_style(OutlineStyle value) { m_noninherited.outline_style = value; }
    void set_outline_width(CSSPixels value) { m_noninherited.outline_width = value; }
    void set_mask(MaskReference const& value) { m_noninherited.mask = value; }
    void set_mask_type(MaskType value) { m_noninherited.mask_type = value; }
    void set_mask_image(AbstractImageStyleValue const& value) { m_noninherited.mask_image = value; }
    void set_clip_path(ClipPathReference value) { m_noninherited.clip_path = move(value); }
    void set_clip_rule(ClipRule value) { m_inherited.clip_rule = value; }
    void set_flood_color(Color value) { m_noninherited.flood_color = value; }
    void set_flood_opacity(float value) { m_noninherited.flood_opacity = value; }
    void set_shape_rendering(ShapeRendering value) { m_noninherited.shape_rendering = value; }
    void set_paint_order(PaintOrderList value) { m_inherited.paint_order = value; }

    void set_cx(LengthPercentage cx) { m_noninherited.cx = move(cx); }
    void set_cy(LengthPercentage cy) { m_noninherited.cy = move(cy); }
    void set_r(LengthPercentage r) { m_noninherited.r = move(r); }
    void set_rx(LengthPercentage rx) { m_noninherited.rx = move(rx); }
    void set_ry(LengthPercentage ry) { m_noninherited.ry = move(ry); }
    void set_x(LengthPercentage x) { m_noninherited.x = move(x); }
    void set_y(LengthPercentage y) { m_noninherited.y = move(y); }

    void set_math_shift(MathShift value) { m_inherited.math_shift = value; }
    void set_math_style(MathStyle value) { m_inherited.math_style = value; }
    void set_math_depth(int value) { m_inherited.math_depth = value; }

    void set_scrollbar_color(ScrollbarColorData value) { m_inherited.scrollbar_color = move(value); }
    void set_scrollbar_width(ScrollbarWidth value) { m_noninherited.scrollbar_width = value; }

    void set_counter_increment(Vector<CounterData> value) { m_noninherited.counter_increment = move(value); }
    void set_counter_reset(Vector<CounterData> value) { m_noninherited.counter_reset = move(value); }
    void set_counter_set(Vector<CounterData> value) { m_noninherited.counter_set = move(value); }

    void set_will_change(WillChange value) { m_noninherited.will_change = move(value); }
};

}
