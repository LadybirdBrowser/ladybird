/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedBitmap.h>
#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/Utf16FlyString.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/InterpolationColorSpace.h>
#include <LibGfx/ScalingMode.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Clip.h>
#include <LibWeb/CSS/ColumnCount.h>
#include <LibWeb/CSS/CounterStyle.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/CSS/Display.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Filter.h>
#include <LibWeb/CSS/FontFeatureData.h>
#include <LibWeb/CSS/GridTrackPlacement.h>
#include <LibWeb/CSS/GridTrackSize.h>
#include <LibWeb/CSS/LengthBox.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/Ratio.h>
#include <LibWeb/CSS/Size.h>
#include <LibWeb/CSS/StyleStructRef.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/BasicShapeStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/CursorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/Time.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

class Document;

}

namespace Web::CSS {

class AnimatedProperties;

class ComputedProperties;
class StyleScope;

using ClipRule = FillRule;

struct FlexBasisContent {
    bool operator==(FlexBasisContent const&) const = default;
};
using FlexBasis = Variant<FlexBasisContent, Size>;

struct AspectRatio {
    bool use_natural_aspect_ratio_if_available;
    Optional<Ratio> preferred_ratio;
    bool computed_use_natural_aspect_ratio_if_available;
    Optional<Ratio> computed_ratio;

    bool operator==(AspectRatio const& other) const
    {
        // NB: Ratio's own equality is proportional (1 / 2 equals 0.5 / 1), but these ratios are
        //     observable through serialization, so compare the components exactly.
        auto ratios_identical = [](Optional<Ratio> const& a, Optional<Ratio> const& b) {
            if (a.has_value() != b.has_value())
                return false;
            if (!a.has_value())
                return true;
            return a->numerator() == b->numerator() && a->denominator() == b->denominator();
        };
        return use_natural_aspect_ratio_if_available == other.use_natural_aspect_ratio_if_available
            && computed_use_natural_aspect_ratio_if_available == other.computed_use_natural_aspect_ratio_if_available
            && ratios_identical(preferred_ratio, other.preferred_ratio)
            && ratios_identical(computed_ratio, other.computed_ratio);
    }
};

struct AnchorScopeData {
    bool all { false };
    Vector<Utf16FlyString> names;

    bool operator==(AnchorScopeData const&) const = default;
};

struct PositionVisibilityData {
    bool always { false };
    bool anchors_valid { false };
    bool anchors_visible { true };
    bool no_overflow { false };

    bool operator==(PositionVisibilityData const&) const = default;
};

struct PositionAreaData {
    Vector<PositionArea> keywords;
    bool operator==(PositionAreaData const&) const = default;
};

struct PositionTryFallbackData {
    Optional<Utf16FlyString> name;
    Vector<TryTactic> tactics;
    Optional<PositionAreaData> position_area;
    bool operator==(PositionTryFallbackData const&) const = default;
};

struct TimelineScopeData {
    bool all { false };
    Vector<Utf16FlyString> names;
    bool operator==(TimelineScopeData const&) const = default;
};

struct ViewTimelineInsetData {
    LengthPercentageOrAuto start { LengthPercentageOrAuto::make_auto() };
    LengthPercentageOrAuto end { LengthPercentageOrAuto::make_auto() };
    bool operator==(ViewTimelineInsetData const&) const = default;
};

struct AnimationTimelineData {
    enum class Type : u8 {
        Auto,
        None,
        Name,
        Scroll,
        View,
    };

    Type type { Type::Auto };
    Utf16FlyString name;
    Scroller scroller { Scroller::Nearest };
    Axis axis { Axis::Block };
    ViewTimelineInsetData inset;

    bool operator==(AnimationTimelineData const&) const = default;
};

struct ShapeOutsideData {
    Variant<Empty, URL, NonnullRefPtr<AbstractImageStyleValue const>> image;
    RefPtr<BasicShapeStyleValue const> basic_shape;
    Optional<ShapeBox> shape_box;

    bool operator==(ShapeOutsideData const&) const = default;
};

struct GridAutoFlow {
    bool row { true };
    bool dense { false };

    bool operator==(GridAutoFlow const&) const = default;
};

struct NormalGap {
    bool operator==(NormalGap const&) const = default;
};

struct QuotesData {
    enum class Type {
        None,
        Auto,
        Specified,
    } type;
    Vector<Array<Utf16FlyString, 2>> strings {};

    bool operator==(QuotesData const&) const = default;
};

struct Position {
    PositionEdge edge_x { PositionEdge::Left };
    LengthPercentage offset_x { Percentage(50) };
    PositionEdge edge_y { PositionEdge::Top };
    LengthPercentage offset_y { Percentage(50) };

    bool operator==(Position const&) const = default;

    CSSPixelPoint resolved(CSSPixelRect const& rect) const
    {
        CSSPixels x = offset_x.to_px(rect.width());
        CSSPixels y = offset_y.to_px(rect.height());
        if (edge_x == PositionEdge::Right)
            x = rect.width() - x;
        if (edge_y == PositionEdge::Bottom)
            y = rect.height() - y;
        return CSSPixelPoint { rect.x() + x, rect.y() + y };
    }
};

struct PositionAnchor {
    enum class Type : u8 {
        Normal,
        None,
        Auto,
        Name,
    };

    Type type { Type::Normal };
    Optional<Utf16FlyString> name;

    bool operator==(PositionAnchor const&) const = default;
};

// https://drafts.csswg.org/css-contain-2/#containment-types
struct Containment {
    bool size_containment : 1 { false };
    bool inline_size_containment : 1 { false };
    bool layout_containment : 1 { false };
    bool style_containment : 1 { false };
    bool paint_containment : 1 { false };

    bool is_empty() const { return !(size_containment || inline_size_containment || layout_containment || style_containment || paint_containment); }

    bool operator==(Containment const&) const = default;
};

struct ContainerType {
    bool is_size_container : 1 { false };
    bool is_inline_size_container : 1 { false };
    bool is_scroll_state_container : 1 { false };

    bool operator==(ContainerType const&) const = default;

    bool is_empty() const { return !(is_size_container || is_inline_size_container || is_scroll_state_container); }
};

struct ScrollbarColorData {
    Color thumb_color { Color::Transparent };
    Color track_color { Color::Transparent };
    bool is_auto { true };

    bool operator==(ScrollbarColorData const&) const = default;
};

struct TextIndentData {
    LengthPercentage length_percentage;
    bool each_line { false };
    bool hanging { false };

    bool operator==(TextIndentData const&) const = default;
};

struct TextUnderlinePosition {
    TextUnderlinePositionHorizontal horizontal { TextUnderlinePositionHorizontal::Auto };
    TextUnderlinePositionVertical vertical { TextUnderlinePositionVertical::Auto };

    bool operator==(TextUnderlinePosition const&) const = default;
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
    bool operator==(WillChange const&) const = default;
    bool has_property(PropertyID property_id) const { return m_value.contains_slow(property_id); }
    Vector<WillChangeEntry> const& entries() const { return m_value; }

private:
    WillChange()
    {
    }

    Vector<WillChangeEntry> m_value;
};

using CursorData = Variant<NonnullRefPtr<CursorStyleValue const>, CursorPredefined>;

struct OverflowClipMarginSide {
    Optional<BackgroundBox> visual_box {};
    CSSPixels offset { 0 };

    bool operator==(OverflowClipMarginSide const&) const = default;
};

struct OverflowClipMarginData {
    OverflowClipMarginSide left;
    OverflowClipMarginSide top;
    OverflowClipMarginSide right;
    OverflowClipMarginSide bottom;

    bool operator==(OverflowClipMarginData const&) const = default;
};

struct ListStyleSymbols {
    NonnullRefPtr<CounterStyle const> counter_style;
    SymbolsType type;
    Vector<Utf16FlyString> symbols;

    bool operator==(ListStyleSymbols const&) const = default;
};

using ListStyleType = Variant<Empty, RefPtr<CounterStyle const>, Utf16String, Utf16FlyString, ListStyleSymbols>;

struct ComputedFontStyle {
    FontStyleKeyword keyword { FontStyleKeyword::Normal };
    Optional<Variant<Angle, NonnullRefPtr<CalculatedStyleValue const>>> angle;

    bool operator==(ComputedFontStyle const&) const = default;
};

enum class OverflowWrap : u8 {
    Normal,
    BreakWord,
    Anywhere,
};

class InitialValues {
public:
    static AspectRatio aspect_ratio() { return AspectRatio { true, {}, true, {} }; }
    static CSSPixels font_size() { return 16; }
    static double font_weight() { return 400; }
    static Percentage font_width() { return Percentage(100); }
    static FontOpticalSizing font_optical_sizing() { return FontOpticalSizing::Auto; }
    static ComputedFontStyle font_style() { return {}; }
    static FontFeatureData font_feature_data()
    {
        return {};
    }
    static CSSPixels line_height() { return 0; }
    static Float float_() { return Float::None; }
    static CSSPixels border_spacing() { return 0; }
    static CaptionSide caption_side() { return CaptionSide::Top; }
    static Color caret_color() { return Color::Black; }
    static Clear clear() { return Clear::None; }
    static Clip clip() { return Clip::make_auto(); }
    static ColorInterpolation color_interpolation() { return ColorInterpolation::Srgb; }
    static ColorInterpolation color_interpolation_filters() { return ColorInterpolation::Linearrgb; }
    static PreferredColorScheme color_scheme() { return PreferredColorScheme::Auto; }
    static ContentVisibility content_visibility() { return ContentVisibility::Visible; }
    static CursorData cursor() { return { CursorPredefined::Auto }; }
    static WhiteSpaceCollapse white_space_collapse() { return WhiteSpaceCollapse::Collapse; }
    static WordBreak word_break() { return WordBreak::Normal; }
    static FontVariantEmoji font_variant_emoji() { return FontVariantEmoji::Normal; }
    static CSSPixels word_spacing() { return 0; }
    static CSSPixels letter_spacing() { return 0; }
    static TextAlign text_align() { return TextAlign::Start; }
    static TextJustify text_justify() { return TextJustify::Auto; }
    static Positioning position() { return Positioning::Static; }
    static PositionAnchor position_anchor() { return {}; }
    static PositionAreaData position_area() { return {}; }
    static Vector<PositionTryFallbackData> position_try_fallbacks() { return {}; }
    static Optional<TryOrder> position_try_order() { return {}; }
    static PositionVisibilityData position_visibility() { return {}; }
    static TimelineScopeData timeline_scope() { return {}; }
    static TextDecorationLine text_decoration_line() { return TextDecorationLine::None; }
    static TextDecorationSkipInk text_decoration_skip_ink() { return TextDecorationSkipInk::Auto; }
    static TextDecorationStyle text_decoration_style() { return TextDecorationStyle::Solid; }
    static TextTransform text_transform() { return TextTransform::None; }
    static TextIndentData text_indent() { return { Length::make_px(0) }; }
    static TextWrapMode text_wrap_mode() { return TextWrapMode::Wrap; }
    static TextWrapStyle text_wrap_style() { return TextWrapStyle::Auto; }
    static CSSPixels text_underline_offset() { return 2; }
    static OverflowWrap overflow_wrap() { return OverflowWrap::Normal; }
    static u64 orphans() { return 2; }
    static u64 widows() { return 2; }
    static TextUnderlinePosition text_underline_position() { return { .horizontal = TextUnderlinePositionHorizontal::Auto, .vertical = TextUnderlinePositionVertical::Auto }; }
    static Display display() { return Display { DisplayOutside::Inline, DisplayInside::Flow }; }
    static Color color() { return Color::Black; }
    static Color stop_color() { return Color::Black; }
    static Filter backdrop_filter() { return Filter::make_none(); }
    static Filter filter() { return Filter::make_none(); }
    static Color background_color() { return Color::Transparent; }
    static BackgroundBox background_color_clip() { return BackgroundBox::BorderBox; }
    static ListStyleType list_style_type() { return RefPtr<CounterStyle const> { CounterStyle::disc() }; }
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
    static VectorEffect vector_effect() { return VectorEffect::None; }
    static float stroke_miterlimit() { return 4.0f; }
    static float stroke_opacity() { return 1.0f; }
    static LengthPercentage stroke_width() { return Length::make_px(1); }
    static float stop_opacity() { return 1.0f; }
    static TextAnchor text_anchor() { return TextAnchor::Start; }
    static LengthPercentage border_radius() { return LengthPercentage { Length::make_px(0) }; }
    static Variant<VerticalAlign, LengthPercentage> vertical_align() { return VerticalAlign::Baseline; }
    static LengthBox inset() { return {}; }
    static LengthBox margin() { return { Length::make_px(0), Length::make_px(0), Length::make_px(0), Length::make_px(0) }; }
    static LengthBox padding() { return { Length::make_px(0), Length::make_px(0), Length::make_px(0), Length::make_px(0) }; }
    static LengthBox scroll_margin() { return { Length::make_px(0), Length::make_px(0), Length::make_px(0), Length::make_px(0) }; }
    static LengthBox scroll_padding() { return {}; }
    static OverflowClipMarginData overflow_clip_margin() { return {}; }
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
    static Variant<LengthPercentage, NormalGap> column_gap() { return NormalGap {}; }
    static ColumnSpan column_span() { return ColumnSpan::None; }
    static Size column_height() { return Size::make_auto(); }
    static Variant<LengthPercentage, NormalGap> row_gap() { return NormalGap {}; }
    static BorderCollapse border_collapse() { return BorderCollapse::Separate; }
    static EmptyCells empty_cells() { return EmptyCells::Show; }
    static GridTemplateAreas grid_template_areas() { return {}; }
    static ObjectFit object_fit() { return ObjectFit::Fill; }
    static Position object_position() { return {}; }
    static Color outline_color() { return Color::Black; }
    static CSSPixels outline_offset() { return 0; }
    static OutlineStyle outline_style() { return OutlineStyle::None; }
    static CSSPixels outline_width() { return 3; }
    static QuotesData quotes() { return QuotesData { .type = QuotesData::Type::Auto }; }
    static TransformBox transform_box() { return TransformBox::ViewBox; }
    static TransformStyle transform_style() { return TransformStyle::Flat; }
    static Direction direction() { return Direction::Ltr; }
    static Optional<BaselineMetric> dominant_baseline() { return {}; }
    static WritingMode writing_mode() { return WritingMode::HorizontalTb; }
    static UserSelect user_select() { return UserSelect::Auto; }
    static Isolation isolation() { return Isolation::Auto; }
    static Containment contain() { return {}; }
    static Vector<Utf16FlyString> container_name() { return {}; }
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

    static ScrollBehavior scroll_behavior() { return ScrollBehavior::Auto; }
    static ScrollbarColorData scrollbar_color()
    {
        return ScrollbarColorData {
            .thumb_color = Color(Color::NamedColor::DarkGray).with_alpha(192),
            .track_color = Color(Color::NamedColor::WarmGray).with_alpha(192),
            .is_auto = true,
        };
    }
    static ScrollbarGutter scrollbar_gutter() { return ScrollbarGutter::Auto; }
    static ScrollbarWidth scrollbar_width() { return ScrollbarWidth::Auto; }
    static Resize resize() { return Resize::None; }
    static double shape_image_threshold() { return 0; }
    static LengthPercentage shape_margin() { return Length::make_px(0); }
    static ShapeOutsideData shape_outside() { return {}; }
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
    static SVGPaint from_style_value(NonnullRefPtr<StyleValue const> const& style_value, ColorResolutionContext const& color_resolution_context)
    {
        if (style_value->has_color())
            return { style_value->to_color(color_resolution_context).value(), style_value->to_keyword() == Keyword::Currentcolor };

        if (style_value->is_value_list()) {
            auto const& values = style_value->as_value_list().values();

            VERIFY(values.size() == 2);

            if (values[1]->is_empty_optional())
                return values[0]->as_url().url();

            return { values[0]->as_url().url(), values[1]->to_color(color_resolution_context), values[1]->to_keyword() == Keyword::Currentcolor };
        }

        VERIFY_NOT_REACHED();
    }

    SVGPaint(Color color, bool color_is_currentcolor = false)
        : m_value(color)
        , m_color_is_currentcolor(color_is_currentcolor)
    {
    }
    SVGPaint(URL const& url, Optional<Color> fallback_color = {}, bool fallback_color_is_currentcolor = false)
        : m_value(url)
        , m_fallback_color(fallback_color)
        , m_color_is_currentcolor(fallback_color_is_currentcolor)
    {
    }

    bool is_color() const { return m_value.has<Color>(); }
    bool is_url() const { return m_value.has<URL>(); }
    Color as_color() const { return m_value.get<Color>(); }
    URL const& as_url() const { return m_value.get<URL>(); }
    Optional<Color> const& fallback_color() const { return m_fallback_color; }
    bool color_is_currentcolor() const { return m_color_is_currentcolor; }

public:
    bool operator==(SVGPaint const&) const = default;

private:
    Variant<URL, Color> m_value;
    Optional<Color> m_fallback_color;
    bool m_color_is_currentcolor { false };
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

    bool operator==(MaskReference const&) const = default;

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

    bool operator==(ClipPathReference const&) const = default;

private:
    using BasicShape = NonnullRefPtr<BasicShapeStyleValue const>;

    Variant<URL, BasicShape> m_clip_source;
};

struct BackgroundLayerData {
    RefPtr<AbstractImageStyleValue const> background_image;
    RefPtr<StyleValue const> image_style_value;
    BackgroundAttachment attachment { BackgroundAttachment::Scroll };
    BackgroundBox origin { BackgroundBox::PaddingBox };
    BackgroundBox clip { BackgroundBox::BorderBox };
    LengthPercentage position_x { Percentage(0) };
    LengthPercentage position_y { Percentage(0) };
    BackgroundSize size_type { BackgroundSize::LengthPercentage };
    LengthPercentageOrAuto size_x { LengthPercentageOrAuto::make_auto() };
    LengthPercentageOrAuto size_y { LengthPercentageOrAuto::make_auto() };
    Repetition repeat_x { Repetition::Repeat };
    Repetition repeat_y { Repetition::Repeat };
    MixBlendMode blend_mode { MixBlendMode::Normal };
    bool mask_clip_is_no_clip { false };
    CoordBox mask_clip { CoordBox::BorderBox };
    CompositingOperator mask_composite { CompositingOperator::Add };
    MaskingMode mask_mode { MaskingMode::MatchSource };
    CoordBox mask_origin { CoordBox::BorderBox };

    bool operator==(BackgroundLayerData const&) const = default;
};

struct BorderImageWidthAuto {
    bool operator==(BorderImageWidthAuto const&) const = default;
};

using BorderImageSliceValue = Variant<double, Percentage, NonnullRefPtr<CalculatedStyleValue const>>;
using BorderImageWidthValue = Variant<double, LengthPercentage, BorderImageWidthAuto>;
using BorderImageOutsetValue = Variant<double, Length>;

template<typename T>
struct BorderImageSideValues {
    T top;
    T right;
    T bottom;
    T left;

    bool operator==(BorderImageSideValues const&) const = default;
};

struct BorderImageData {
    RefPtr<AbstractImageStyleValue const> source;
    BorderImageSideValues<BorderImageSliceValue> slice { Percentage(100), Percentage(100), Percentage(100), Percentage(100) };
    BorderImageSideValues<BorderImageWidthValue> width { 1.0, 1.0, 1.0, 1.0 };
    BorderImageSideValues<BorderImageOutsetValue> outset { 0.0, 0.0, 0.0, 0.0 };
    u8 width_value_count { 1 };
    u8 outset_value_count { 1 };
    bool fill { false };
    BorderImageRepeat repeat_x { BorderImageRepeat::Stretch };
    BorderImageRepeat repeat_y { BorderImageRepeat::Stretch };

    bool operator==(BorderImageData const&) const = default;
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

    bool operator==(TouchActionData const&) const = default;

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

    bool operator==(WhiteSpaceTrimData const&) const = default;
};

struct TransformOrigin {
    LengthPercentage x { Percentage(50) };
    LengthPercentage y { Percentage(50) };
    // FIXME: We can store this as a CSSPixels since we know it's always an absolute length
    LengthPercentage z { Percentage(0) };

    bool operator==(TransformOrigin const&) const = default;
};

struct ShadowData {
    CSSPixels offset_x { 0 };
    CSSPixels offset_y { 0 };
    CSSPixels blur_radius { 0 };
    CSSPixels spread_distance { 0 };
    Color color {};
    ColorSyntax color_syntax { ColorSyntax::Legacy };
    ShadowPlacement placement { ShadowPlacement::Outer };

    bool operator==(ShadowData const&) const = default;
};

struct ContentData {
    enum class Type {
        Normal,
        None,
        List,
    } type { Type::Normal };

    Vector<Variant<Utf16String, NonnullRefPtr<AbstractImageStyleValue>>> data;
    Vector<ValueComparingRefPtr<CounterStyle const>> counter_style_dependencies;
    Optional<Utf16String> alt_text {};

    bool operator==(ContentData const&) const = default;
};

struct ContentDataAndQuoteNestingLevel {
    ContentData content_data;
    u32 final_quote_nesting_level { 0 };
};

struct ComputedContentCounter {
    enum class Function : u8 {
        Counter,
        Counters,
    };

    struct SymbolsFunction {
        SymbolsType type;
        Vector<Utf16FlyString> symbols;

        bool operator==(SymbolsFunction const&) const = default;
    };

    Function function;
    Utf16FlyString name;
    Utf16FlyString join_string;
    Variant<Utf16FlyString, SymbolsFunction> style;

    bool operator==(ComputedContentCounter const&) const = default;
};

using ComputedContentItem = Variant<Utf16String, Keyword, ComputedContentCounter, NonnullRefPtr<AbstractImageStyleValue const>>;

struct ComputedContentData {
    enum class Type : u8 {
        Normal,
        None,
        List,
    };

    Type type { Type::Normal };
    Vector<ComputedContentItem> items;
    Vector<ComputedContentItem> alt_text;

    bool operator==(ComputedContentData const&) const = default;
};

struct CounterData {
    Utf16FlyString name;
    bool is_reversed;
    Optional<CounterValue> value;

    bool operator==(CounterData const&) const = default;
};

struct BorderRadiusData {
    LengthPercentage horizontal_radius { InitialValues::border_radius() };
    LengthPercentage vertical_radius { InitialValues::border_radius() };

    [[nodiscard]] bool is_initial() const
    {
        return horizontal_radius.is_length() && horizontal_radius.length().is_px() && horizontal_radius.length().absolute_length_to_px() == 0
            && vertical_radius.is_length() && vertical_radius.length().is_px() && vertical_radius.length().absolute_length_to_px() == 0;
    }

    bool operator==(BorderRadiusData const&) const = default;
};

enum class ComputedFontFamilySyntax {
    CustomIdent,
    String,
};

struct ComputedFontFamilyName {
    Utf16FlyString name;
    ComputedFontFamilySyntax syntax { ComputedFontFamilySyntax::CustomIdent };

    bool operator==(ComputedFontFamilyName const&) const = default;
};

using ComputedFontFamily = Variant<GenericFontFamily, ComputedFontFamilyName>;

enum class ComputedAnimationNameSyntax {
    None,
    CustomIdent,
    String,
};

struct ComputedAnimationName {
    Utf16FlyString name;
    ComputedAnimationNameSyntax syntax { ComputedAnimationNameSyntax::None };

    bool operator==(ComputedAnimationName const&) const = default;
};

struct TextDecorationThickness {
    struct Auto {
        bool operator==(Auto const&) const = default;
    };
    struct FromFont {
        bool operator==(FromFont const&) const = default;
    };
    Variant<Auto, FromFont, LengthPercentage> value;

    bool operator==(TextDecorationThickness const&) const = default;
};

struct ColorOrAuto {
    struct Auto {
        bool operator==(Auto const&) const = default;
    };

    Variant<Auto, Color> computed_value { Auto {} };
    Color used_value { Color::Black };

    bool operator==(ColorOrAuto const&) const = default;
};

struct TextUnderlineOffset {
    struct Auto {
        bool operator==(Auto const&) const = default;
    };

    Variant<Auto, LengthPercentage> computed_value { Auto {} };
    CSSPixels used_value { 2 };

    bool operator==(TextUnderlineOffset const&) const = default;
};

struct LineHeightData {
    struct Normal {
        bool operator==(Normal const&) const = default;
    };

    Variant<Normal, double, Length> computed_value { Normal {} };

    bool operator==(LineHeightData const&) const = default;
};

// FIXME: Find a better place for this helper.
inline Gfx::ScalingMode to_gfx_scaling_mode(ImageRendering css_value, Gfx::IntSize source, Gfx::IntSize target)
{
    switch (css_value) {
    case ImageRendering::Auto:
    case ImageRendering::HighQuality:
    case ImageRendering::Optimizequality:
    case ImageRendering::Smooth:
        if (target.width() < source.width() && target.height() < source.height())
            return Gfx::ScalingMode::BilinearMipmap;
        return Gfx::ScalingMode::Bilinear;
    case ImageRendering::CrispEdges:
    case ImageRendering::Optimizespeed:
    case ImageRendering::Pixelated:
        return Gfx::ScalingMode::NearestNeighbor;
    }
    VERIFY_NOT_REACHED();
}

// FIXME: Find a better place for this helper.
inline Gfx::InterpolationColorSpace to_interpolation_color_space(ColorInterpolation css_value)
{
    switch (css_value) {
    case ColorInterpolation::Linearrgb:
        return Gfx::InterpolationColorSpace::LinearRGB;
    case ColorInterpolation::Auto:
    case ColorInterpolation::Srgb:
        return Gfx::InterpolationColorSpace::SRGB;
    }
    VERIFY_NOT_REACHED();
}

// The identity of every ComputedValues style value group, in vtable registration order.
#define LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(G) \
    G(InheritedTableValues)                             \
    G(InheritedListValues)                              \
    G(InheritedUIValues)                                \
    G(InheritedSVGValues)                               \
    G(InheritedTextValues)                              \
    G(InheritedBoxValues)                               \
    G(FontValues)                                       \
    G(AnimationValues)                                  \
    G(SVGResetValues)                                   \
    G(GridValues)                                       \
    G(AnchorValues)                                     \
    G(EffectsValues)                                    \
    G(MaskValues)                                       \
    G(TextResetValues)                                  \
    G(ContentValues)                                    \
    G(TransformValues)                                  \
    G(BackgroundValues)                                 \
    G(BorderValues)                                     \
    G(AlignmentValues)                                  \
    G(MiscResetValues)                                  \
    G(SizingValues)                                     \
    G(SurroundValues)                                   \
    G(BoxValues)

enum class StyleGroupIndex : size_t {
#define LIBWEB_STYLE_GROUP_ENUMERATOR(name) name,
    LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(LIBWEB_STYLE_GROUP_ENUMERATOR)
#undef LIBWEB_STYLE_GROUP_ENUMERATOR
        Count,
};

// The box group payload stores display values in the Rust-defined explicit
// form; these pins keep the tag discriminants aligned with Display::Type.
inline ComputedValuesFFI::FfiDisplay to_ffi_display(Display const& display)
{
    static_assert(to_underlying(Display::Type::OutsideAndInside) == 0);
    static_assert(to_underlying(Display::Type::Internal) == 1);
    static_assert(to_underlying(Display::Type::Box) == 2);

    switch (display.type()) {
    case Display::Type::OutsideAndInside:
        return {
            .tag = to_underlying(display.type()),
            .outside = to_underlying(display.outside()),
            .inside = to_underlying(display.inside()),
            .list_item = display.is_list_item(),
            .internal = 0,
            .box_value = 0,
        };
    case Display::Type::Internal:
        return {
            .tag = to_underlying(display.type()),
            .outside = 0,
            .inside = 0,
            .list_item = false,
            .internal = to_underlying(display.internal()),
            .box_value = 0,
        };
    case Display::Type::Box:
        return {
            .tag = to_underlying(display.type()),
            .outside = 0,
            .inside = 0,
            .list_item = false,
            .internal = 0,
            .box_value = display.is_none() ? to_underlying(DisplayBox::None) : to_underlying(DisplayBox::Contents),
        };
    }
    VERIFY_NOT_REACHED();
}

inline Display display_from_ffi_display(ComputedValuesFFI::FfiDisplay const& display)
{
    switch (static_cast<Display::Type>(display.tag)) {
    case Display::Type::OutsideAndInside:
        return Display {
            static_cast<DisplayOutside>(display.outside),
            static_cast<DisplayInside>(display.inside),
            display.list_item ? Display::ListItem::Yes : Display::ListItem::No,
        };
    case Display::Type::Internal:
        return Display { static_cast<DisplayInternal>(display.internal) };
    case Display::Type::Box:
        return Display { static_cast<DisplayBox>(display.box_value) };
    }
    VERIFY_NOT_REACHED();
}

inline ComputedValuesFFI::ComputedAspectRatio to_ffi_aspect_ratio(AspectRatio const& aspect_ratio)
{
    return {
        .use_natural_aspect_ratio_if_available = aspect_ratio.use_natural_aspect_ratio_if_available,
        .has_preferred_ratio = aspect_ratio.preferred_ratio.has_value(),
        .preferred_ratio_numerator = aspect_ratio.preferred_ratio.has_value() ? aspect_ratio.preferred_ratio->numerator() : 0.0,
        .preferred_ratio_denominator = aspect_ratio.preferred_ratio.has_value() ? aspect_ratio.preferred_ratio->denominator() : 0.0,
        .computed_use_natural_aspect_ratio_if_available = aspect_ratio.computed_use_natural_aspect_ratio_if_available,
        .has_computed_ratio = aspect_ratio.computed_ratio.has_value(),
        .computed_ratio_numerator = aspect_ratio.computed_ratio.has_value() ? aspect_ratio.computed_ratio->numerator() : 0.0,
        .computed_ratio_denominator = aspect_ratio.computed_ratio.has_value() ? aspect_ratio.computed_ratio->denominator() : 0.0,
    };
}

inline ComputedValuesFFI::ComputedVerticalAlign to_ffi_vertical_align(Variant<VerticalAlign, LengthPercentage> const& value)
{
    if (value.has<VerticalAlign>())
        return { .is_keyword = true, .keyword = to_underlying(value.get<VerticalAlign>()), .value = { nullptr } };
    auto retained = value.get<LengthPercentage>();
    return { .is_keyword = false, .keyword = 0, .value = { retained.leak_data() } };
}

// The returned raw struct carries the by-value Size's retained reference for
// a Rust-owned payload to assume ownership of.
inline ComputedValuesFFI::ComputedSize to_ffi_computed_size(Size size)
{
    return { .kind = size.kind, .value = { exchange(size.value.pointer, nullptr) } };
}

// Each returned raw carries one leaked reference for a Rust-owned fly string
// list to assume ownership of.
inline Vector<size_t> to_leaked_fly_string_raws(Vector<Utf16FlyString> const& names)
{
    Vector<size_t> raws;
    raws.ensure_capacity(names.size());
    for (auto const& name : names)
        raws.unchecked_append(name.to_raw_leaked());
    return raws;
}

class WEB_API ComputedValues final : public RefCounted<ComputedValues> {
    AK_MAKE_NONCOPYABLE(ComputedValues);
    AK_MAKE_NONMOVABLE(ComputedValues);

public:
    class Builder;
    class Mutator;

    enum class WithAnimationsApplied {
        No,
        Yes,
    };

    static NonnullRefPtr<ComputedValues const> create(ComputedProperties const&, DOM::Document const&, StyleScope const&, ColorResolutionContext, ComputedValues const* inherit_parent = nullptr);

    RefPtr<StyleValue const> computed_style_value(PropertyID, WithAnimationsApplied = WithAnimationsApplied::Yes) const;
    RefPtr<StyleValue const> computed_style_value_for_inheritance(PropertyID, WithAnimationsApplied = WithAnimationsApplied::Yes) const;
    RefPtr<StyleValue const> color_style_value() const;
    ComputedValues const& base_values() const { return m_base_values ? *m_base_values : *this; }
    bool has_animated_values() const { return m_base_values; }
    AnimatedProperties const* animated_properties() const { return m_animated_properties.ptr(); }
    RefPtr<AnimatedProperties const> animated_properties_snapshot() const;

    struct Statistics {
        u64 live_instance_count { 0 };
        u64 total_instances_created { 0 };
    };
    static Statistics const& statistics() { return s_statistics; }

    // Shares group payloads with `previous` wherever the values compare equal. This changes no
    // observable value, only the identity of the backing payloads, so it is safe on an otherwise
    // immutable ComputedValues. It makes pointer-based diffing hit on the next restyle and lets a
    // restyled element keep sharing storage across style generations. Returns true when every
    // group ends up sharing its payload with `previous`.
    bool adopt_identical_group_payloads(ComputedValues const& previous) const;

    // Returns the Rust-owned payload for direct read-only layout access. The
    // pointer is borrowed from this immutable ComputedValues instance.
    void const* style_group_payload(StyleGroupIndex) const;

    // Fills one payload pointer per StyleGroupIndex, in enum order.
    void fill_style_group_payloads(Span<void const*>) const;

    // Calls back with (name, shared_with_parent, is_default) for every style value group,
    // for introspecting how well group sharing is working (see internals.styleGroupSharingInfo()).
    template<typename Callback>
    void for_each_style_group_sharing_state(ComputedValues const* parent, Callback callback) const
    {
#define LIBWEB_VISIT_STYLE_GROUP(name, path) callback(name##sv, parent ? path.ptr_equals(parent->path) : false, path.is_default());
        LIBWEB_VISIT_STYLE_GROUP("inheritedTable", m_inherited.table)
        LIBWEB_VISIT_STYLE_GROUP("inheritedList", m_inherited.list)
        LIBWEB_VISIT_STYLE_GROUP("inheritedUI", m_inherited.ui)
        LIBWEB_VISIT_STYLE_GROUP("inheritedSVG", m_inherited.svg)
        LIBWEB_VISIT_STYLE_GROUP("inheritedText", m_inherited.text)
        LIBWEB_VISIT_STYLE_GROUP("inheritedBox", m_inherited.box)
        LIBWEB_VISIT_STYLE_GROUP("font", m_inherited.font)
        LIBWEB_VISIT_STYLE_GROUP("animation", m_noninherited.animation)
        LIBWEB_VISIT_STYLE_GROUP("box", m_noninherited.box)
        LIBWEB_VISIT_STYLE_GROUP("surround", m_noninherited.surround)
        LIBWEB_VISIT_STYLE_GROUP("sizing", m_noninherited.sizing)
        LIBWEB_VISIT_STYLE_GROUP("miscReset", m_noninherited.misc)
        LIBWEB_VISIT_STYLE_GROUP("alignment", m_noninherited.alignment)
        LIBWEB_VISIT_STYLE_GROUP("border", m_noninherited.border)
        LIBWEB_VISIT_STYLE_GROUP("background", m_noninherited.background)
        LIBWEB_VISIT_STYLE_GROUP("transform", m_noninherited.transform)
        LIBWEB_VISIT_STYLE_GROUP("effects", m_noninherited.effects)
        LIBWEB_VISIT_STYLE_GROUP("mask", m_noninherited.mask_data)
        LIBWEB_VISIT_STYLE_GROUP("textReset", m_noninherited.text_reset)
        LIBWEB_VISIT_STYLE_GROUP("content", m_noninherited.content_data)
        LIBWEB_VISIT_STYLE_GROUP("anchor", m_noninherited.anchor)
        LIBWEB_VISIT_STYLE_GROUP("grid", m_noninherited.grid)
        LIBWEB_VISIT_STYLE_GROUP("svgReset", m_noninherited.svg_reset)
#undef LIBWEB_VISIT_STYLE_GROUP
    }

    bool is_property_important(PropertyID property_id) const { return m_property_important.get(property_bitmap_index(property_id)); }
    bool is_property_inherited(PropertyID property_id) const { return m_property_inherited.get(property_bitmap_index(property_id)); }
    bool depends_on_viewport_metrics() const { return m_depends_on_viewport_metrics; }
    bool font_metrics_depend_on_viewport_metrics() const { return m_font_metrics_depend_on_viewport_metrics; }
    bool in_display_none_subtree() const { return m_in_display_none_subtree; }
    bool has_pseudo_element_style(PseudoElement pseudo_element) const { return m_pseudo_element_styles & (1ull << to_underlying(pseudo_element)); }
    HashMap<PropertyID, NonnullRefPtr<StyleValue const>> const& inheritance_dependent_specified_values() const { return m_inheritance_dependent_specified_values; }
    RefPtr<StyleValue const> raw_cascaded_font_size() const { return m_raw_cascaded_font_size; }

    ~ComputedValues();

    AspectRatio aspect_ratio() const
    {
        auto const& value = m_noninherited.box->aspect_ratio;
        return AspectRatio {
            value.use_natural_aspect_ratio_if_available,
            value.has_preferred_ratio ? Optional<Ratio> { Ratio { value.preferred_ratio_numerator, value.preferred_ratio_denominator } } : OptionalNone {},
            value.computed_use_natural_aspect_ratio_if_available,
            value.has_computed_ratio ? Optional<Ratio> { Ratio { value.computed_ratio_numerator, value.computed_ratio_denominator } } : OptionalNone {},
        };
    }
    Vector<Utf16FlyString> const& anchor_names() const { return m_noninherited.anchor->anchor_names; }
    AnchorScopeData const& anchor_scope() const { return m_noninherited.anchor->anchor_scope; }
    Vector<ComputedAnimationName> const& animation_names() const { return m_noninherited.animation->animation_names; }
    Vector<AnimationComposition> const& animation_compositions() const { return m_noninherited.animation->animation_compositions; }
    Vector<Time> const& animation_delays() const { return m_noninherited.animation->animation_delays; }
    Vector<AnimationDirection> const& animation_directions() const { return m_noninherited.animation->animation_directions; }
    Vector<Optional<Time>> const& animation_durations() const { return m_noninherited.animation->animation_durations; }
    Vector<AnimationFillMode> const& animation_fill_modes() const { return m_noninherited.animation->animation_fill_modes; }
    Vector<double> const& animation_iteration_counts() const { return m_noninherited.animation->animation_iteration_counts; }
    Vector<AnimationPlayState> const& animation_play_states() const { return m_noninherited.animation->animation_play_states; }
    Vector<AnimationTimelineData> const& animation_timelines() const { return m_noninherited.animation->animation_timelines; }
    Vector<EasingFunction> const& animation_timing_functions() const { return m_noninherited.animation->animation_timing_functions; }
    StyleValueVector const& animation_timing_function_style_values() const { return m_noninherited.animation->animation_timing_function_style_values; }
    BoxSizing box_sizing_for_aspect_ratio() const
    {
        // https://drafts.csswg.org/css-sizing-4/#aspect-ratio
        // For a preferred aspect ratio specified as `auto && <ratio>`, the ratio is applied to the content box.
        if (aspect_ratio().use_natural_aspect_ratio_if_available)
            return BoxSizing::ContentBox;
        return box_sizing();
    }

    Float float_() const { return static_cast<Float>(m_noninherited.box->float_); }
    CSSPixels border_spacing_horizontal() const { return CSSPixels::from_raw(m_inherited.table->border_spacing_horizontal); }
    CSSPixels border_spacing_vertical() const { return CSSPixels::from_raw(m_inherited.table->border_spacing_vertical); }
    CaptionSide caption_side() const { return static_cast<CaptionSide>(m_inherited.table->caption_side); }
    ColorOrAuto const& caret_color_value() const { return m_inherited.ui->caret_color; }
    Color caret_color() const { return m_inherited.ui->caret_color.used_value; }
    Clear clear() const { return static_cast<Clear>(m_noninherited.box->clear); }
    Clip clip() const { return m_noninherited.effects->clip; }
    ColorInterpolation color_interpolation() const { return m_inherited.svg->color_interpolation; }
    ColorInterpolation color_interpolation_filters() const { return m_inherited.svg->color_interpolation_filters; }
    PreferredColorScheme color_scheme() const { return m_inherited.ui->color_scheme; }
    Vector<Utf16FlyString> const& color_schemes() const { return m_inherited.ui->color_schemes; }
    bool color_scheme_only() const { return m_inherited.ui->color_scheme_only; }
    ContentVisibility content_visibility() const { return static_cast<ContentVisibility>(m_inherited.box->content_visibility); }
    Vector<CursorData> const& cursor() const { return m_inherited.ui->cursor; }
    Optional<ContentData> const& content() const { return m_noninherited.content_data->content; }
    ComputedContentData const& computed_content() const { return m_noninherited.content_data->computed_content; }
    ContentDataAndQuoteNestingLevel resolved_content(DOM::AbstractElement&, u32 initial_quote_nesting_level) const;
    Vector<CounterData, 0> const& counter_increment() const { return m_noninherited.content_data->counter_increment; }
    Vector<CounterData, 0> const& counter_reset() const { return m_noninherited.content_data->counter_reset; }
    Vector<CounterData, 0> const& counter_set() const { return m_noninherited.content_data->counter_set; }
    PointerEvents pointer_events() const { return m_inherited.ui->pointer_events; }
    Display display() const { return display_from_ffi_display(m_noninherited.box->display); }
    Display display_before_box_type_transformation() const { return display_from_ffi_display(m_noninherited.box->display_before_box_type_transformation); }
    Optional<int> z_index() const
    {
        if (!m_noninherited.box->has_z_index)
            return {};
        return m_noninherited.box->z_index;
    }
    Variant<CSSPixels, double> tab_size() const
    {
        if (m_inherited.text->tab_size_is_number)
            return m_inherited.text->tab_size_number;
        return m_inherited.text->tab_size_length;
    }
    TextAlign text_align() const { return m_inherited.text->text_align; }
    TextJustify text_justify() const { return m_inherited.text->text_justify; }
    TextIndentData const& text_indent() const { return m_inherited.text->text_indent; }
    TextWrapMode text_wrap_mode() const { return m_inherited.text->text_wrap_mode; }
    TextWrapStyle text_wrap_style() const { return m_inherited.text->text_wrap_style; }
    TextUnderlineOffset const& text_underline_offset_value() const { return m_inherited.text->text_underline_offset; }
    CSSPixels text_underline_offset() const { return m_inherited.text->text_underline_offset.used_value; }
    TextUnderlinePosition text_underline_position() const { return m_inherited.text->text_underline_position; }
    Vector<TextDecorationLine> const& text_decoration_line() const { return m_noninherited.text_reset->text_decoration_line; }
    TextDecorationThickness const& text_decoration_thickness() const { return m_noninherited.text_reset->text_decoration_thickness; }
    TextDecorationSkipInk text_decoration_skip_ink() const { return m_inherited.text->text_decoration_skip_ink; }
    TextDecorationStyle text_decoration_style() const { return m_noninherited.text_reset->text_decoration_style; }
    Color text_decoration_color() const { return m_noninherited.text_reset->text_decoration_color; }
    TextTransform text_transform() const { return m_inherited.text->text_transform; }
    TextOverflow text_overflow() const { return static_cast<TextOverflow>(m_noninherited.box->text_overflow); }
    Vector<ShadowData> const& text_shadow() const { return m_inherited.text->text_shadow; }
    Positioning position() const { return static_cast<Positioning>(m_noninherited.box->position); }
    PositionAnchor const& position_anchor_value() const { return m_noninherited.anchor->position_anchor; }
    Optional<Utf16FlyString> const& position_anchor() const { return m_noninherited.anchor->position_anchor.name; }
    PositionAreaData const& position_area() const { return m_noninherited.anchor->position_area; }
    Vector<PositionTryFallbackData> const& position_try_fallbacks() const { return m_noninherited.anchor->position_try_fallbacks; }
    Optional<TryOrder> position_try_order() const { return m_noninherited.anchor->position_try_order; }
    PositionVisibilityData const& position_visibility() const { return m_noninherited.anchor->position_visibility; }
    Vector<Optional<Utf16FlyString>> const& scroll_timeline_names() const { return m_noninherited.animation->scroll_timeline_names; }
    Vector<Axis> const& scroll_timeline_axes() const { return m_noninherited.animation->scroll_timeline_axes; }
    TimelineScopeData const& timeline_scope() const { return m_noninherited.animation->timeline_scope; }
    Vector<Optional<Utf16FlyString>> const& view_timeline_names() const { return m_noninherited.animation->view_timeline_names; }
    Vector<Axis> const& view_timeline_axes() const { return m_noninherited.animation->view_timeline_axes; }
    Vector<ViewTimelineInsetData> const& view_timeline_insets() const { return m_noninherited.animation->view_timeline_insets; }
    Vector<Optional<Utf16FlyString>> const& transition_properties() const { return m_noninherited.animation->transition_properties; }
    Vector<Time> const& transition_durations() const { return m_noninherited.animation->transition_durations; }
    Vector<EasingFunction> const& transition_timing_functions() const { return m_noninherited.animation->transition_timing_functions; }
    StyleValueVector const& transition_timing_function_style_values() const { return m_noninherited.animation->transition_timing_function_style_values; }
    Vector<Time> const& transition_delays() const { return m_noninherited.animation->transition_delays; }
    Vector<TransitionBehavior> const& transition_behaviors() const { return m_noninherited.animation->transition_behaviors; }
    WhiteSpaceCollapse white_space_collapse() const { return m_inherited.text->white_space_collapse; }
    WhiteSpaceTrimData white_space_trim() const { return m_noninherited.text_reset->white_space_trim; }
    WordBreak word_break() const { return m_inherited.text->word_break; }
    OverflowWrap overflow_wrap() const { return m_inherited.text->overflow_wrap; }
    u64 orphans() const { return m_inherited.text->orphans; }
    u64 widows() const { return m_inherited.text->widows; }
    FontVariantEmoji font_variant_emoji() const { return m_inherited.font->font_variant_emoji; }
    CSSPixels const& word_spacing() const { return m_inherited.text->word_spacing; }
    CSSPixels letter_spacing() const { return m_inherited.text->letter_spacing; }
    RefPtr<StyleValue const> word_spacing_style_value() const;
    RefPtr<StyleValue const> letter_spacing_style_value() const;
    FlexDirection flex_direction() const { return static_cast<FlexDirection>(m_noninherited.alignment->flex_direction); }
    FlexWrap flex_wrap() const { return static_cast<FlexWrap>(m_noninherited.alignment->flex_wrap); }
    FlexBasis flex_basis() const
    {
        if (m_noninherited.alignment->flex_basis.is_content)
            return FlexBasisContent {};
        return Size::view(m_noninherited.alignment->flex_basis.size);
    }
    double flex_grow() const { return m_noninherited.alignment->flex_grow; }
    double flex_shrink() const { return m_noninherited.alignment->flex_shrink; }
    i32 order() const { return m_noninherited.alignment->order; }
    ColorOrAuto const& accent_color_value() const { return m_inherited.ui->accent_color; }
    Optional<Color> accent_color() const
    {
        if (m_inherited.ui->accent_color.computed_value.has<ColorOrAuto::Auto>())
            return {};
        return m_inherited.ui->accent_color.used_value;
    }
    AlignContent align_content() const { return static_cast<AlignContent>(m_noninherited.alignment->align_content); }
    AlignItems align_items() const { return static_cast<AlignItems>(m_noninherited.alignment->align_items); }
    AlignSelf align_self() const { return static_cast<AlignSelf>(m_noninherited.alignment->align_self); }
    Appearance appearance() const { return m_noninherited.misc->appearance; }
    Appearance computed_appearance() const { return m_noninherited.misc->computed_appearance; }
    float opacity() const { return m_noninherited.effects->opacity; }
    Visibility visibility() const { return static_cast<Visibility>(m_inherited.box->visibility); }
    ImageRendering image_rendering() const { return static_cast<ImageRendering>(m_inherited.box->image_rendering); }
    JustifyContent justify_content() const { return static_cast<JustifyContent>(m_noninherited.alignment->justify_content); }
    JustifySelf justify_self() const { return static_cast<JustifySelf>(m_noninherited.alignment->justify_self); }
    JustifyItems justify_items() const { return static_cast<JustifyItems>(m_noninherited.alignment->justify_items); }
    Filter const& backdrop_filter() const { return m_noninherited.effects->backdrop_filter; }
    Filter const& filter() const { return m_noninherited.effects->filter; }
    Vector<ShadowData> const& box_shadow() const { return m_noninherited.effects->box_shadow; }
    BoxSizing box_sizing() const { return static_cast<BoxSizing>(m_noninherited.box->box_sizing); }
    Size const& width() const { return Size::view(m_noninherited.sizing->width); }
    Size const& min_width() const { return Size::view(m_noninherited.sizing->min_width); }
    Size const& max_width() const { return Size::view(m_noninherited.sizing->max_width); }
    Size const& height() const { return Size::view(m_noninherited.sizing->height); }
    Size const& min_height() const { return Size::view(m_noninherited.sizing->min_height); }
    Size const& max_height() const { return Size::view(m_noninherited.sizing->max_height); }
    Variant<VerticalAlign, LengthPercentage> vertical_align() const
    {
        auto const& value = m_noninherited.box->vertical_align;
        if (value.is_keyword)
            return static_cast<VerticalAlign>(value.keyword);
        return LengthPercentage::view(value.value);
    }
    GridTrackSizeList const& grid_auto_columns() const { return m_noninherited.grid->grid_auto_columns; }
    GridTrackSizeList const& grid_auto_rows() const { return m_noninherited.grid->grid_auto_rows; }
    GridAutoFlow grid_auto_flow() const
    {
        return { .row = m_noninherited.box->grid_auto_flow_row, .dense = m_noninherited.box->grid_auto_flow_dense };
    }
    GridTrackSizeList const& grid_template_columns() const { return m_noninherited.grid->grid_template_columns; }
    GridTrackSizeList const& grid_template_rows() const { return m_noninherited.grid->grid_template_rows; }
    GridTrackPlacement const& grid_column_end() const { return m_noninherited.grid->grid_column_end; }
    GridTrackPlacement const& grid_column_start() const { return m_noninherited.grid->grid_column_start; }
    GridTrackPlacement const& grid_row_end() const { return m_noninherited.grid->grid_row_end; }
    GridTrackPlacement const& grid_row_start() const { return m_noninherited.grid->grid_row_start; }
    ColumnCount column_count() const
    {
        if (!m_noninherited.box->column_count_has_value)
            return ColumnCount::make_auto();
        return ColumnCount::make_integer(m_noninherited.box->column_count);
    }
    Variant<LengthPercentage, NormalGap> column_gap() const { return gap(m_noninherited.alignment->column_gap); }
    ColumnSpan const& column_span() const { return m_noninherited.misc->column_span; }
    Size const& column_width() const { return Size::view(m_noninherited.box->column_width); }
    Size const& column_height() const { return m_noninherited.misc->column_height; }
    Variant<LengthPercentage, NormalGap> row_gap() const { return gap(m_noninherited.alignment->row_gap); }
    BorderCollapse border_collapse() const { return static_cast<BorderCollapse>(m_inherited.table->border_collapse); }
    EmptyCells empty_cells() const { return static_cast<EmptyCells>(m_inherited.table->empty_cells); }
    GridTemplateAreas const& grid_template_areas() const { return m_noninherited.grid->grid_template_areas; }
    ObjectFit object_fit() const { return m_noninherited.misc->object_fit; }
    Position object_position() const { return m_noninherited.misc->object_position; }
    Direction direction() const { return static_cast<Direction>(m_inherited.box->direction); }
    Optional<BaselineMetric> dominant_baseline() const { return m_inherited.svg->dominant_baseline; }
    UnicodeBidi unicode_bidi() const { return static_cast<UnicodeBidi>(m_noninherited.box->unicode_bidi); }
    WritingMode writing_mode() const { return static_cast<WritingMode>(m_inherited.box->writing_mode); }

    bool inline_axis_is_reverse() const
    {
        switch (writing_mode()) {
        case WritingMode::HorizontalTb:
        case WritingMode::VerticalRl:
        case WritingMode::VerticalLr:
        case WritingMode::SidewaysRl:
            return direction() == Direction::Rtl;
        case WritingMode::SidewaysLr:
            return direction() == Direction::Ltr;
        }
        VERIFY_NOT_REACHED();
    }

    bool block_axis_is_reverse() const
    {
        switch (writing_mode()) {
        case WritingMode::HorizontalTb:
        case WritingMode::VerticalLr:
        case WritingMode::SidewaysLr:
            return false;
        case WritingMode::VerticalRl:
        case WritingMode::SidewaysRl:
            return true;
        }
        VERIFY_NOT_REACHED();
    }

    UserSelect user_select() const { return m_noninherited.misc->user_select; }
    Isolation isolation() const { return m_noninherited.effects->isolation; }
    Containment contain() const
    {
        auto const& box = *m_noninherited.box;
        return Containment {
            .size_containment = box.size_containment,
            .inline_size_containment = box.inline_size_containment,
            .layout_containment = box.layout_containment,
            .style_containment = box.style_containment,
            .paint_containment = box.paint_containment,
        };
    }
    bool container_name_contains(Utf16FlyString const& name) const
    {
        auto const& list = m_noninherited.box->container_name;
        for (size_t i = 0; i < list.length; ++i) {
            if (Utf16FlyString::from_raw(list.pointer[i].raw) == name)
                return true;
        }
        return false;
    }
    Vector<Utf16FlyString> container_name() const
    {
        auto const& list = m_noninherited.box->container_name;
        Vector<Utf16FlyString> names;
        names.ensure_capacity(list.length);
        for (size_t i = 0; i < list.length; ++i)
            names.unchecked_append(Utf16FlyString::from_raw(list.pointer[i].raw));
        return names;
    }
    ContainerType container_type() const
    {
        auto const& box = *m_noninherited.box;
        return ContainerType {
            .is_size_container = box.is_size_container,
            .is_inline_size_container = box.is_inline_size_container,
            .is_scroll_state_container = box.is_scroll_state_container,
        };
    }
    MixBlendMode mix_blend_mode() const { return m_noninherited.effects->mix_blend_mode; }
    Optional<Utf16FlyString> view_transition_name() const { return m_noninherited.misc->view_transition_name; }
    TouchActionData touch_action() const { return m_noninherited.misc->touch_action; }
    ShapeRendering shape_rendering() const { return static_cast<ShapeRendering>(m_noninherited.svg_reset->shape_rendering); }

    LengthBox inset() const { return length_box(m_noninherited.surround->inset); }
    RefPtr<StyleValue const> anchor_inset(PropertyID property_id) const
    {
        ComputedValuesFFI::ComputedStyleValueHandle const* handle = nullptr;
        switch (property_id) {
        case PropertyID::Top:
            handle = &m_noninherited.surround->top_anchor_inset;
            break;
        case PropertyID::Right:
            handle = &m_noninherited.surround->right_anchor_inset;
            break;
        case PropertyID::Bottom:
            handle = &m_noninherited.surround->bottom_anchor_inset;
            break;
        case PropertyID::Left:
            handle = &m_noninherited.surround->left_anchor_inset;
            break;
        default:
            return {};
        }
        static_assert(sizeof(RustStyleValueHandle) == sizeof(*handle));
        return style_value_from_handle(property_id, reinterpret_cast<RustStyleValueHandle const&>(*handle));
    }
    LengthBox margin() const { return length_box(m_noninherited.surround->margin); }
    LengthBox padding() const { return length_box(m_noninherited.surround->padding); }
    LengthBox const& scroll_margin() const { return m_noninherited.misc->scroll_margin; }
    LengthBox const& scroll_padding() const { return m_noninherited.misc->scroll_padding; }
    OverflowClipMarginData const& overflow_clip_margin() const { return m_noninherited.misc->overflow_clip_margin; }

    BorderData const& border_left() const { return m_noninherited.border->border_left; }
    BorderData const& border_top() const { return m_noninherited.border->border_top; }
    BorderData const& border_right() const { return m_noninherited.border->border_right; }
    BorderData const& border_bottom() const { return m_noninherited.border->border_bottom; }
    CSSPixels border_left_computed_width() const { return m_noninherited.border->border_left_computed_width; }
    CSSPixels border_top_computed_width() const { return m_noninherited.border->border_top_computed_width; }
    CSSPixels border_right_computed_width() const { return m_noninherited.border->border_right_computed_width; }
    CSSPixels border_bottom_computed_width() const { return m_noninherited.border->border_bottom_computed_width; }

    bool has_noninitial_border_radii() const { return m_noninherited.border->has_noninitial_border_radii; }
    BorderRadiusData const& border_bottom_left_radius() const { return m_noninherited.border->border_bottom_left_radius; }
    BorderRadiusData const& border_bottom_right_radius() const { return m_noninherited.border->border_bottom_right_radius; }
    BorderRadiusData const& border_top_left_radius() const { return m_noninherited.border->border_top_left_radius; }
    BorderRadiusData const& border_top_right_radius() const { return m_noninherited.border->border_top_right_radius; }
    double corner_bottom_left_shape() const { return m_noninherited.border->corner_bottom_left_shape; }
    double corner_bottom_right_shape() const { return m_noninherited.border->corner_bottom_right_shape; }
    double corner_top_left_shape() const { return m_noninherited.border->corner_top_left_shape; }
    double corner_top_right_shape() const { return m_noninherited.border->corner_top_right_shape; }

    Overflow overflow_x() const { return static_cast<Overflow>(m_noninherited.box->overflow_x); }
    Overflow overflow_y() const { return static_cast<Overflow>(m_noninherited.box->overflow_y); }

    Color color() const { return m_inherited.text->color; }
    Color background_color() const { return m_noninherited.background->background_color; }
    RefPtr<StyleValue const> background_color_style_value() const;
    BackgroundBox background_color_clip() const { return m_noninherited.background->background_color_clip; }
    Vector<BackgroundLayerData> const& background_layers() const { return m_noninherited.background->background_layers; }
    Vector<BackgroundLayerData> const& mask_layers() const { return m_noninherited.mask_data->mask_layers; }
    Vector<Position> const& mask_positions() const { return m_noninherited.mask_data->mask_positions; }
    BorderImageData const& border_image() const { return m_noninherited.border->border_image; }

    Color webkit_text_fill_color() const { return m_inherited.text->webkit_text_fill_color; }
    bool webkit_text_fill_color_is_current_color() const { return m_inherited.text->webkit_text_fill_color_is_current_color; }

    ListStyleType const& list_style_type() const { return m_inherited.list->list_style_type; }
    ListStylePosition list_style_position() const { return m_inherited.list->list_style_position; }
    AbstractImageStyleValue const* list_style_image() const { return m_inherited.list->list_style_image.ptr(); }

    Optional<SVGPaint> const& fill() const { return m_inherited.svg->fill; }
    FillRule fill_rule() const { return m_inherited.svg->fill_rule; }
    Optional<SVGPaint> const& stroke() const { return m_inherited.svg->stroke; }
    float fill_opacity() const { return m_inherited.svg->fill_opacity; }
    Vector<Variant<LengthPercentage, float>> const& stroke_dasharray() const { return m_inherited.svg->stroke_dasharray; }
    LengthPercentage const& stroke_dashoffset() const { return m_inherited.svg->stroke_dashoffset; }
    StrokeLinecap stroke_linecap() const { return m_inherited.svg->stroke_linecap; }
    StrokeLinejoin stroke_linejoin() const { return m_inherited.svg->stroke_linejoin; }
    VectorEffect vector_effect() const { return static_cast<VectorEffect>(m_noninherited.svg_reset->vector_effect); }
    double stroke_miterlimit() const { return m_inherited.svg->stroke_miterlimit; }
    float stroke_opacity() const { return m_inherited.svg->stroke_opacity; }
    LengthPercentage const& stroke_width() const { return m_inherited.svg->stroke_width; }
    Color stop_color() const { return Gfx::Color::from_bgra(m_noninherited.svg_reset->stop_color); }
    float stop_opacity() const { return m_noninherited.svg_reset->stop_opacity; }
    TextAnchor text_anchor() const { return m_inherited.svg->text_anchor; }
    RefPtr<AbstractImageStyleValue const> mask_image() const { return m_noninherited.mask_data->mask_image; }
    Optional<MaskReference> const& mask() const { return m_noninherited.mask_data->mask; }
    MaskType mask_type() const { return m_noninherited.mask_data->mask_type; }
    Optional<ClipPathReference> const& clip_path() const { return m_noninherited.mask_data->clip_path; }
    ClipRule clip_rule() const { return m_inherited.svg->clip_rule; }
    Color flood_color() const { return Gfx::Color::from_bgra(m_noninherited.svg_reset->flood_color); }
    float flood_opacity() const { return m_noninherited.svg_reset->flood_opacity; }
    PaintOrderList paint_order() const { return m_inherited.svg->paint_order; }
    u8 paint_order_serialization_length() const { return m_inherited.svg->paint_order_serialization_length; }
    bool paint_order_is_normal() const { return m_inherited.svg->paint_order_is_normal; }

    LengthPercentage const& cx() const { return LengthPercentage::view(m_noninherited.svg_reset->cx); }
    LengthPercentage const& cy() const { return LengthPercentage::view(m_noninherited.svg_reset->cy); }
    LengthPercentage const& r() const { return LengthPercentage::view(m_noninherited.svg_reset->r); }
    LengthPercentageOrAuto rx() const { return m_noninherited.svg_reset->rx.is_auto ? LengthPercentageOrAuto::make_auto() : LengthPercentage::view(m_noninherited.svg_reset->rx.value); }
    LengthPercentageOrAuto ry() const { return m_noninherited.svg_reset->ry.is_auto ? LengthPercentageOrAuto::make_auto() : LengthPercentage::view(m_noninherited.svg_reset->ry.value); }
    LengthPercentage const& x() const { return LengthPercentage::view(m_noninherited.svg_reset->x); }
    LengthPercentage const& y() const { return LengthPercentage::view(m_noninherited.svg_reset->y); }

    Vector<NonnullRefPtr<TransformationStyleValue const>> const& transformations() const { return m_noninherited.transform->transformations; }
    TransformBox const& transform_box() const { return m_noninherited.transform->transform_box; }
    TransformOrigin const& transform_origin() const { return m_noninherited.transform->transform_origin; }
    TransformStyle const& transform_style() const { return m_noninherited.transform->transform_style; }
    RefPtr<TransformationStyleValue const> const& rotate() const { return m_noninherited.transform->rotate; }
    RefPtr<TransformationStyleValue const> const& translate() const { return m_noninherited.transform->translate; }
    RefPtr<TransformationStyleValue const> const& scale() const { return m_noninherited.transform->scale; }
    Optional<CSSPixels> const& perspective() const { return m_noninherited.transform->perspective; }
    Position const& perspective_origin() const { return m_noninherited.transform->perspective_origin; }

    Gfx::FontCascadeList const& font_list() const { return *m_inherited.font->font_list; }
    Vector<ComputedFontFamily> const& font_families() const { return m_inherited.font->font_families; }
    CSSPixels font_size() const { return m_inherited.font->font_size; }
    double font_weight() const { return m_inherited.font->font_weight; }
    Percentage font_width() const { return m_inherited.font->font_width; }
    ComputedFontStyle const& font_style() const { return m_inherited.font->font_style; }
    FontOpticalSizing font_optical_sizing() const { return m_inherited.font->font_optical_sizing; }
    FontFeatureData const& font_feature_data() const { return m_inherited.font->font_feature_data; }
    Optional<Utf16FlyString> font_language_override() const { return m_inherited.font->font_language_override; }
    HashMap<Utf16FlyString, double> font_variation_settings() const { return m_inherited.font->font_variation_settings; }
    CSSPixels line_height() const { return m_inherited.font->line_height_used; }
    LineHeightData const& line_height_data() const { return m_inherited.font->line_height; }

    Color outline_color() const { return m_noninherited.misc->outline_color; }
    CSSPixels outline_offset() const { return m_noninherited.misc->outline_offset; }
    RefPtr<StyleValue const> outline_offset_style_value() const { return m_noninherited.misc->outline_offset_style_value; }
    OutlineStyle outline_style() const { return m_noninherited.misc->outline_style; }
    CSSPixels outline_width() const { return m_noninherited.misc->outline_width; }

    TableLayout table_layout() const { return static_cast<TableLayout>(m_noninherited.box->table_layout); }

    QuotesData quotes() const { return m_inherited.list->quotes; }

    MathShift math_shift() const { return m_inherited.font->math_shift; }
    MathStyle math_style() const { return m_inherited.font->math_style; }
    int math_depth() const { return m_inherited.font->math_depth; }

    ScrollBehavior scroll_behavior() const { return m_noninherited.misc->scroll_behavior; }
    ScrollbarColorData scrollbar_color() const { return m_inherited.ui->scrollbar_color; }
    ScrollbarGutter scrollbar_gutter() const { return m_noninherited.misc->scrollbar_gutter; }
    ScrollbarWidth scrollbar_width() const { return m_noninherited.misc->scrollbar_width; }
    Resize resize() const { return static_cast<Resize>(m_noninherited.box->resize); }
    double shape_image_threshold() const { return m_noninherited.misc->shape_image_threshold; }
    LengthPercentage const& shape_margin() const { return m_noninherited.misc->shape_margin; }
    ShapeOutsideData const& shape_outside() const { return m_noninherited.misc->shape_outside; }
    WillChange const& will_change() const { return m_noninherited.misc->will_change; }

private:
    ComputedValues();

    RefPtr<StyleValue const> style_value_from_handle(PropertyID, RustStyleValueHandle const&) const;

    static LengthPercentageOrAuto length_percentage_or_auto(ComputedValuesFFI::ComputedLengthPercentageOrAuto const& value)
    {
        if (value.is_auto)
            return LengthPercentageOrAuto::make_auto();
        return LengthPercentage::view(value.value);
    }

    static LengthBox length_box(ComputedValuesFFI::ComputedLengthBox const& box)
    {
        return {
            length_percentage_or_auto(box.top),
            length_percentage_or_auto(box.right),
            length_percentage_or_auto(box.bottom),
            length_percentage_or_auto(box.left),
        };
    }

    static RustStyleValueHandle retain_style_value_data(StyleValue const* value)
    {
        if (!value)
            return {};
        return RustStyleValueHandle { StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()) };
    }

    static Statistics s_statistics;

    static size_t property_bitmap_index(PropertyID property_id)
    {
        VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);
        return to_underlying(property_id) - to_underlying(first_longhand_property_id);
    }

    void inherit_from(ComputedValues const& other) { m_inherited = other.m_inherited; }

public:
    // The layout and lifecycle of this group are defined in Rust (computed_values.rs).
    struct InheritedTableValues : ComputedValuesFFI::InheritedTableValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::InheritedTableValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::InheritedTable;

        bool operator==(InheritedTableValues const& other) const
        {
            return border_collapse == other.border_collapse
                && caption_side == other.caption_side
                && empty_cells == other.empty_cells
                && border_spacing_horizontal == other.border_spacing_horizontal
                && border_spacing_vertical == other.border_spacing_vertical;
        }
    };

    struct InheritedListValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::InheritedListValues);
        ListStyleType list_style_type { InitialValues::list_style_type() };
        ListStylePosition list_style_position { InitialValues::list_style_position() };
        RefPtr<AbstractImageStyleValue const> list_style_image;
        QuotesData quotes { InitialValues::quotes() };

        bool operator==(InheritedListValues const&) const = default;
    };

    struct InheritedUIValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::InheritedUIValues);
        ColorOrAuto caret_color;
        ColorOrAuto accent_color;
        Vector<CursorData> cursor { InitialValues::cursor() };
        PointerEvents pointer_events { InitialValues::pointer_events() };
        ScrollbarColorData scrollbar_color { InitialValues::scrollbar_color() };
        PreferredColorScheme color_scheme { InitialValues::color_scheme() };
        Vector<Utf16FlyString> color_schemes;
        bool color_scheme_only { false };

        bool operator==(InheritedUIValues const&) const = default;
    };

    struct InheritedSVGValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::InheritedSVGValues);
        Optional<SVGPaint> fill;
        Optional<SVGPaint> stroke;
        FillRule fill_rule { InitialValues::fill_rule() };
        ClipRule clip_rule { InitialValues::clip_rule() };
        float fill_opacity { InitialValues::fill_opacity() };
        float stroke_opacity { InitialValues::stroke_opacity() };
        StrokeLinecap stroke_linecap { InitialValues::stroke_linecap() };
        StrokeLinejoin stroke_linejoin { InitialValues::stroke_linejoin() };
        Vector<Variant<LengthPercentage, float>> stroke_dasharray;
        LengthPercentage stroke_dashoffset { InitialValues::stroke_dashoffset() };
        double stroke_miterlimit { InitialValues::stroke_miterlimit() };
        LengthPercentage stroke_width { InitialValues::stroke_width() };
        ColorInterpolation color_interpolation { InitialValues::color_interpolation() };
        ColorInterpolation color_interpolation_filters { InitialValues::color_interpolation_filters() };
        PaintOrderList paint_order { InitialValues::paint_order() };
        u8 paint_order_serialization_length { 0 };
        bool paint_order_is_normal { true };
        TextAnchor text_anchor { InitialValues::text_anchor() };
        Optional<BaselineMetric> dominant_baseline { InitialValues::dominant_baseline() };

        static InheritedSVGValues make_default_payload_value();

        bool operator==(InheritedSVGValues const&) const = default;
    };

    // The group's payload lifecycle stays in C++, but the members up to and
    // including text_indent mirror the Rust InheritedTextLayoutFacts prefix,
    // pinned by static asserts in ComputedValues.cpp, so layout reads them as
    // typed fields. The computed tab-size is stored as an explicit
    // length-or-number triple because a Variant has no FFI-stable layout.
    struct InheritedTextValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::InheritedTextValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::CppWithInheritedTextFacts;
        TextAlign text_align { InitialValues::text_align() };
        TextJustify text_justify { InitialValues::text_justify() };
        WhiteSpaceCollapse white_space_collapse { InitialValues::white_space_collapse() };
        TextWrapMode text_wrap_mode { InitialValues::text_wrap_mode() };
        WordBreak word_break { InitialValues::word_break() };
        bool tab_size_is_number { true };
        CSSPixels letter_spacing { InitialValues::letter_spacing() };
        CSSPixels word_spacing { InitialValues::word_spacing() };
        CSSPixels tab_size_length { 0 };
        double tab_size_number { 8 };
        TextIndentData text_indent { InitialValues::text_indent() };
        Color color { InitialValues::color() };
        RustStyleValueHandle color_style_value;
        Color webkit_text_fill_color { InitialValues::color() };
        bool webkit_text_fill_color_is_current_color { true };
        Vector<ShadowData> text_shadow;
        TextTransform text_transform { InitialValues::text_transform() };
        TextWrapStyle text_wrap_style { InitialValues::text_wrap_style() };
        TextDecorationSkipInk text_decoration_skip_ink { InitialValues::text_decoration_skip_ink() };
        TextUnderlinePosition text_underline_position { InitialValues::text_underline_position() };
        TextUnderlineOffset text_underline_offset;
        OverflowWrap overflow_wrap { InitialValues::overflow_wrap() };
        RustStyleValueHandle word_spacing_style_value;
        RustStyleValueHandle letter_spacing_style_value;
        u64 orphans { InitialValues::orphans() };
        u64 widows { InitialValues::widows() };

        bool operator==(InheritedTextValues const&) const = default;
    };

    // The layout and lifecycle of this group are defined in Rust (computed_values.rs). The
    // fields hold the underlying values of the corresponding C++ enums, and the lens getters
    // and setters convert.
    struct InheritedBoxValues : ComputedValuesFFI::InheritedBoxValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::InheritedBoxValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::InheritedBox;

        bool operator==(InheritedBoxValues const& other) const
        {
            return visibility == other.visibility
                && direction == other.direction
                && writing_mode == other.writing_mode
                && content_visibility == other.content_visibility
                && image_rendering == other.image_rendering;
        }
    };

    // NB: FontValues has no defaulted equality operator because HashMap does not
    //     support equality; the setters compare field-by-field instead.
    //
    // The group's payload lifecycle stays in C++, but the members up to and
    // including font_list mirror the Rust FontLayoutFacts prefix, pinned by
    // static asserts in ComputedValues.cpp, so layout reads them as typed
    // fields. The metrics and first_available_font are derived copies that
    // set_font_list keeps in sync with font_list; the derived pointers
    // borrow objects font_list keeps alive.
    struct FontValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::FontValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::CppWithFontFacts;
        CSSPixels font_size { InitialValues::font_size() };
        CSSPixels line_height_used { InitialValues::line_height() };
        FontVariantEmoji font_variant_emoji { InitialValues::font_variant_emoji() };
        float font_ascent { 0 };
        float font_descent { 0 };
        float font_x_height { 0 };
        Gfx::Font const* first_available_font { nullptr };
        RefPtr<Gfx::FontCascadeList const> font_list {};
        Vector<ComputedFontFamily> font_families { GenericFontFamily::Serif };
        double font_weight { InitialValues::font_weight() };
        Percentage font_width { InitialValues::font_width() };
        ComputedFontStyle font_style { InitialValues::font_style() };
        FontOpticalSizing font_optical_sizing { InitialValues::font_optical_sizing() };
        FontFeatureData font_feature_data { InitialValues::font_feature_data() };
        Optional<Utf16FlyString> font_language_override;
        HashMap<Utf16FlyString, double> font_variation_settings;
        LineHeightData line_height;
        MathShift math_shift { InitialValues::math_shift() };
        MathStyle math_style { InitialValues::math_style() };
        int math_depth { InitialValues::math_depth() };

        bool operator==(FontValues const&) const;
    };

private:
    struct InheritedValues {
        StyleStructRef<InheritedTableValues> table;
        StyleStructRef<InheritedListValues> list;
        StyleStructRef<InheritedUIValues> ui;
        StyleStructRef<InheritedSVGValues> svg;
        StyleStructRef<InheritedTextValues> text;
        StyleStructRef<InheritedBoxValues> box;
        StyleStructRef<FontValues> font;
    };

    InheritedValues m_inherited;

public:
    struct AnimationValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::AnimationValues);
        Vector<ComputedAnimationName> animation_names { ComputedAnimationName {} };
        Vector<AnimationComposition> animation_compositions { AnimationComposition::Replace };
        Vector<Time> animation_delays { Time::make_seconds(0) };
        Vector<AnimationDirection> animation_directions { AnimationDirection::Normal };
        Vector<Optional<Time>> animation_durations { Optional<Time> {} };
        Vector<AnimationFillMode> animation_fill_modes { AnimationFillMode::None };
        Vector<double> animation_iteration_counts { 1 };
        Vector<AnimationPlayState> animation_play_states { AnimationPlayState::Running };
        Vector<AnimationTimelineData> animation_timelines { AnimationTimelineData {} };
        Vector<EasingFunction> animation_timing_functions { EasingFunction::ease() };
        StyleValueVector animation_timing_function_style_values;
        Vector<Optional<Utf16FlyString>> scroll_timeline_names { Optional<Utf16FlyString> {} };
        Vector<Axis> scroll_timeline_axes { Axis::Block };
        TimelineScopeData timeline_scope { InitialValues::timeline_scope() };
        Vector<Optional<Utf16FlyString>> view_timeline_names { Optional<Utf16FlyString> {} };
        Vector<Axis> view_timeline_axes { Axis::Block };
        Vector<ViewTimelineInsetData> view_timeline_insets { ViewTimelineInsetData {} };
        Vector<Optional<Utf16FlyString>> transition_properties { Optional<Utf16FlyString> { "all"_utf16_fly_string } };
        Vector<Time> transition_durations { Time::make_seconds(0) };
        Vector<EasingFunction> transition_timing_functions { EasingFunction::ease() };
        StyleValueVector transition_timing_function_style_values;
        Vector<Time> transition_delays { Time::make_seconds(0) };
        Vector<TransitionBehavior> transition_behaviors { TransitionBehavior::Normal };

        static AnimationValues make_default_payload_value();

        bool operator==(AnimationValues const&) const = default;
    };

    // The layout and lifecycle of this group are defined in Rust (computed_values.rs).
    struct SVGResetValues : ComputedValuesFFI::SVGResetValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::SVGResetValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::SVGReset;

        bool operator==(SVGResetValues const& other) const
        {
            auto length_percentage_or_auto_equal = [](auto const& first, auto const& second) {
                if (first.is_auto || second.is_auto)
                    return first.is_auto == second.is_auto;
                return LengthPercentage::view(first.value) == LengthPercentage::view(second.value);
            };
            return LengthPercentage::view(cx) == LengthPercentage::view(other.cx)
                && LengthPercentage::view(cy) == LengthPercentage::view(other.cy)
                && LengthPercentage::view(r) == LengthPercentage::view(other.r)
                && length_percentage_or_auto_equal(rx, other.rx)
                && length_percentage_or_auto_equal(ry, other.ry)
                && LengthPercentage::view(x) == LengthPercentage::view(other.x)
                && LengthPercentage::view(y) == LengthPercentage::view(other.y)
                && stop_color == other.stop_color
                && stop_opacity == other.stop_opacity
                && flood_color == other.flood_color
                && flood_opacity == other.flood_opacity
                && vector_effect == other.vector_effect
                && shape_rendering == other.shape_rendering;
        }
    };

    struct GridValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::GridValues);
        GridTrackSizeList grid_auto_columns;
        GridTrackSizeList grid_auto_rows;
        GridTrackSizeList grid_template_columns;
        GridTrackSizeList grid_template_rows;
        GridTrackPlacement grid_column_end { InitialValues::grid_column_end() };
        GridTrackPlacement grid_column_start { InitialValues::grid_column_start() };
        GridTrackPlacement grid_row_end { InitialValues::grid_row_end() };
        GridTrackPlacement grid_row_start { InitialValues::grid_row_start() };
        GridTemplateAreas grid_template_areas { InitialValues::grid_template_areas() };

        static GridValues make_default_payload_value();

        bool operator==(GridValues const&) const = default;
    };

    struct AnchorValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::AnchorValues);
        Vector<Utf16FlyString> anchor_names;
        AnchorScopeData anchor_scope;
        PositionAnchor position_anchor { InitialValues::position_anchor() };
        PositionAreaData position_area { InitialValues::position_area() };
        Vector<PositionTryFallbackData> position_try_fallbacks { InitialValues::position_try_fallbacks() };
        Optional<TryOrder> position_try_order { InitialValues::position_try_order() };
        PositionVisibilityData position_visibility { InitialValues::position_visibility() };

        bool operator==(AnchorValues const&) const = default;
    };

    struct EffectsValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::EffectsValues);
        float opacity { InitialValues::opacity() };
        Filter filter { InitialValues::filter() };
        Filter backdrop_filter { InitialValues::backdrop_filter() };
        MixBlendMode mix_blend_mode { InitialValues::mix_blend_mode() };
        Isolation isolation { InitialValues::isolation() };
        Vector<ShadowData> box_shadow {};
        Clip clip { InitialValues::clip() };

        bool operator==(EffectsValues const&) const = default;
    };

    struct MaskValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::MaskValues);
        Optional<MaskReference> mask;
        MaskType mask_type { InitialValues::mask_type() };
        RefPtr<AbstractImageStyleValue const> mask_image;
        Vector<BackgroundLayerData> mask_layers { [] {
            BackgroundLayerData layer;
            layer.origin = BackgroundBox::BorderBox;
            layer.clip = BackgroundBox::BorderBox;
            return layer;
        }() };
        Vector<Position> mask_positions { Position { .offset_x = Length::make_px(0), .offset_y = Length::make_px(0) } };
        Optional<ClipPathReference> clip_path;

        static MaskValues make_default_payload_value();

        bool operator==(MaskValues const&) const = default;
    };

    struct TextResetValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::TextResetValues);
        // NB: A computed text-decoration-line of none is the empty list.
        Vector<TextDecorationLine> text_decoration_line {};
        TextDecorationThickness text_decoration_thickness { TextDecorationThickness::Auto {} };
        TextDecorationStyle text_decoration_style { InitialValues::text_decoration_style() };
        Color text_decoration_color { InitialValues::color() };
        WhiteSpaceTrimData white_space_trim;

        bool operator==(TextResetValues const&) const = default;
    };

    struct ContentValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::ContentValues);
        Optional<ContentData> content;
        ComputedContentData computed_content;
        Vector<CounterData, 0> counter_increment;
        Vector<CounterData, 0> counter_reset;
        Vector<CounterData, 0> counter_set;

        bool operator==(ContentValues const&) const = default;
    };

    struct TransformValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::TransformValues);
        Vector<NonnullRefPtr<TransformationStyleValue const>> transformations {};
        TransformBox transform_box { InitialValues::transform_box() };
        TransformOrigin transform_origin {};
        TransformStyle transform_style { InitialValues::transform_style() };
        RefPtr<TransformationStyleValue const> rotate;
        RefPtr<TransformationStyleValue const> translate;
        RefPtr<TransformationStyleValue const> scale;
        Optional<CSSPixels> perspective;
        Position perspective_origin;

        static TransformValues make_default_payload_value();

        bool operator==(TransformValues const&) const = default;
    };

    struct BackgroundValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::BackgroundValues);
        Color background_color { InitialValues::background_color() };
        RustStyleValueHandle background_color_style_value;
        BackgroundBox background_color_clip { InitialValues::background_color_clip() };
        Vector<BackgroundLayerData> background_layers { BackgroundLayerData {} };

        bool operator==(BackgroundValues const&) const = default;
    };

    // The group's payload lifecycle stays in C++, but the four BorderData
    // members lead the struct and mirror the Rust BorderLayoutFacts prefix,
    // pinned by static asserts in ComputedValues.cpp, so layout reads border
    // widths and line styles as typed fields.
    struct BorderValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::BorderValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::CppWithBorderFacts;
        BorderData border_left;
        BorderData border_top;
        BorderData border_right;
        BorderData border_bottom;
        RustStyleValueHandle border_left_color_style_value;
        RustStyleValueHandle border_top_color_style_value;
        RustStyleValueHandle border_right_color_style_value;
        RustStyleValueHandle border_bottom_color_style_value;
        CSSPixels border_left_computed_width { 0 };
        CSSPixels border_top_computed_width { 0 };
        CSSPixels border_right_computed_width { 0 };
        CSSPixels border_bottom_computed_width { 0 };
        bool has_noninitial_border_radii { false };
        BorderRadiusData border_bottom_left_radius;
        BorderRadiusData border_bottom_right_radius;
        BorderRadiusData border_top_left_radius;
        BorderRadiusData border_top_right_radius;
        double corner_bottom_left_shape { 1 };
        double corner_bottom_right_shape { 1 };
        double corner_top_left_shape { 1 };
        double corner_top_right_shape { 1 };
        BorderImageData border_image;

        bool operator==(BorderValues const&) const = default;
    };

    struct AlignmentValues : ComputedValuesFFI::AlignmentValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::AlignmentValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::Alignment;

        bool operator==(AlignmentValues const& other) const
        {
            auto gaps_equal = [](auto const& first, auto const& second) {
                if (first.is_normal || second.is_normal)
                    return first.is_normal == second.is_normal;
                return LengthPercentage::view(first.value) == LengthPercentage::view(second.value);
            };
            return flex_direction == other.flex_direction
                && flex_wrap == other.flex_wrap
                && flex_basis.is_content == other.flex_basis.is_content
                && Size::view(flex_basis.size) == Size::view(other.flex_basis.size)
                && flex_grow == other.flex_grow
                && flex_shrink == other.flex_shrink
                && order == other.order
                && align_content == other.align_content
                && align_items == other.align_items
                && align_self == other.align_self
                && justify_content == other.justify_content
                && justify_items == other.justify_items
                && justify_self == other.justify_self
                && gaps_equal(column_gap, other.column_gap)
                && gaps_equal(row_gap, other.row_gap);
        }
    };

    struct MiscResetValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::MiscResetValues);
        RefPtr<StyleValue const> outline_offset_style_value;
        LengthBox scroll_margin { InitialValues::scroll_margin() };
        LengthBox scroll_padding { InitialValues::scroll_padding() };
        OverflowClipMarginData overflow_clip_margin { InitialValues::overflow_clip_margin() };
        ColumnSpan column_span { InitialValues::column_span() };
        Appearance appearance { InitialValues::appearance() };
        Appearance computed_appearance { Appearance::None };
        OutlineStyle outline_style { InitialValues::outline_style() };
        ObjectFit object_fit { InitialValues::object_fit() };
        Size column_height { InitialValues::column_height() };
        Color outline_color { InitialValues::outline_color() };
        CSSPixels outline_width { InitialValues::outline_width() };
        CSSPixels outline_offset { InitialValues::outline_offset() };
        UserSelect user_select { InitialValues::user_select() };
        Position object_position { InitialValues::object_position() };
        Optional<Utf16FlyString> view_transition_name;
        TouchActionData touch_action;
        ScrollBehavior scroll_behavior { InitialValues::scroll_behavior() };
        ScrollbarGutter scrollbar_gutter { InitialValues::scrollbar_gutter() };
        ScrollbarWidth scrollbar_width { InitialValues::scrollbar_width() };
        double shape_image_threshold { InitialValues::shape_image_threshold() };
        LengthPercentage shape_margin { InitialValues::shape_margin() };
        ShapeOutsideData shape_outside { InitialValues::shape_outside() };
        WillChange will_change { InitialValues::will_change() };

        bool operator==(MiscResetValues const&) const = default;
    };

    struct SizingValues : ComputedValuesFFI::SizingValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::SizingValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::Sizing;

        bool operator==(SizingValues const& other) const
        {
            return Size::view(width) == Size::view(other.width)
                && Size::view(min_width) == Size::view(other.min_width)
                && Size::view(max_width) == Size::view(other.max_width)
                && Size::view(height) == Size::view(other.height)
                && Size::view(min_height) == Size::view(other.min_height)
                && Size::view(max_height) == Size::view(other.max_height);
        }
    };

    struct SurroundValues : ComputedValuesFFI::SurroundValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::SurroundValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::Surround;

        bool operator==(SurroundValues const& other) const
        {
            auto side_equal = [](auto const& first, auto const& second) {
                if (first.is_auto || second.is_auto)
                    return first.is_auto == second.is_auto;
                return LengthPercentage::view(first.value) == LengthPercentage::view(second.value);
            };
            auto box_equal = [&](auto const& first, auto const& second) {
                return side_equal(first.top, second.top)
                    && side_equal(first.right, second.right)
                    && side_equal(first.bottom, second.bottom)
                    && side_equal(first.left, second.left);
            };
            return box_equal(inset, other.inset)
                && top_anchor_inset.pointer == other.top_anchor_inset.pointer
                && right_anchor_inset.pointer == other.right_anchor_inset.pointer
                && bottom_anchor_inset.pointer == other.bottom_anchor_inset.pointer
                && left_anchor_inset.pointer == other.left_anchor_inset.pointer
                && position_anchor_name.raw == other.position_anchor_name.raw
                && box_equal(margin, other.margin)
                && box_equal(padding, other.padding);
        }
    };

    struct BoxValues : ComputedValuesFFI::BoxValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::BoxValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::Box;

        bool operator==(BoxValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
    };

private:
    static Variant<LengthPercentage, NormalGap> gap(ComputedValuesFFI::ComputedGap const& gap)
    {
        if (gap.is_normal)
            return NormalGap {};
        return LengthPercentage::view(gap.value);
    }

    struct NonInheritedValues {
        StyleStructRef<AnimationValues> animation;
        StyleStructRef<BoxValues> box;
        StyleStructRef<SurroundValues> surround;
        StyleStructRef<SizingValues> sizing;
        StyleStructRef<MiscResetValues> misc;
        StyleStructRef<AlignmentValues> alignment;
        StyleStructRef<BorderValues> border;
        StyleStructRef<BackgroundValues> background;
        StyleStructRef<TransformValues> transform;
        StyleStructRef<EffectsValues> effects;
        StyleStructRef<MaskValues> mask_data;
        StyleStructRef<TextResetValues> text_reset;
        StyleStructRef<ContentValues> content_data;
        StyleStructRef<AnchorValues> anchor;
        StyleStructRef<GridValues> grid;
        StyleStructRef<SVGResetValues> svg_reset;
    };

    NonInheritedValues m_noninherited;
    AK::FixedBitmap<number_of_longhand_properties> m_property_important { false };
    AK::FixedBitmap<number_of_longhand_properties> m_property_inherited { false };
    HashMap<PropertyID, NonnullRefPtr<StyleValue const>> m_inheritance_dependent_specified_values;
    mutable HashMap<PropertyID, NonnullRefPtr<StyleValue const>> m_style_value_cache;
    RefPtr<StyleValue const> m_raw_cascaded_font_size;
    RefPtr<ComputedValues const> m_base_values;
    RefPtr<AnimatedProperties const> m_animated_properties;
    u64 m_pseudo_element_styles { 0 };
    bool m_depends_on_viewport_metrics { false };
    bool m_font_metrics_depend_on_viewport_metrics { false };
    bool m_in_display_none_subtree { false };
};

class ComputedValues::Mutator final {
private:
    friend class Builder;

    explicit Mutator(ComputedValues& values)
        : m_values(values)
    {
    }

public:
    void inherit_from(ComputedValues const& other)
    {
        m_values.inherit_from(other);
    }

    void set_property_important(PropertyID property_id, bool value) { m_values.m_property_important.set(ComputedValues::property_bitmap_index(property_id), value); }
    void set_property_inherited(PropertyID property_id, bool value) { m_values.m_property_inherited.set(ComputedValues::property_bitmap_index(property_id), value); }
    void set_depends_on_viewport_metrics(bool value) { m_values.m_depends_on_viewport_metrics = value; }
    void set_font_metrics_depend_on_viewport_metrics(bool value) { m_values.m_font_metrics_depend_on_viewport_metrics = value; }
    void set_in_display_none_subtree(bool value) { m_values.m_in_display_none_subtree = value; }
    void set_pseudo_element_styles(u64 value) { m_values.m_pseudo_element_styles = value; }
    void set_inheritance_dependent_specified_values(HashMap<PropertyID, NonnullRefPtr<StyleValue const>> value) { m_values.m_inheritance_dependent_specified_values = move(value); }
    void set_raw_cascaded_font_size(RefPtr<StyleValue const> value) { m_values.m_raw_cascaded_font_size = move(value); }
    void set_base_values(NonnullRefPtr<ComputedValues const> value) { m_values.m_base_values = move(value); }
    void set_animated_properties(AnimatedProperties const*);

    // Adopts Rust-built group payloads, which arrive already carrying this
    // reference.
    void adopt_inherited_box_group(void* payload) { m_values.m_inherited.box.adopt(payload); }
    void adopt_inherited_table_group(void* payload) { m_values.m_inherited.table.adopt(payload); }
    void adopt_alignment_group(void* payload) { m_values.m_noninherited.alignment.adopt(payload); }
    void adopt_text_reset_group(void* payload) { m_values.m_noninherited.text_reset.adopt(payload); }
    void adopt_effects_group(void* payload) { m_values.m_noninherited.effects.adopt(payload); }
    void adopt_misc_reset_group(void* payload) { m_values.m_noninherited.misc.adopt(payload); }
    void adopt_inherited_text_group(void* payload) { m_values.m_inherited.text.adopt(payload); }
    void adopt_inherited_ui_group(void* payload) { m_values.m_inherited.ui.adopt(payload); }
    void adopt_sizing_group(void* payload) { m_values.m_noninherited.sizing.adopt(payload); }
    void adopt_transform_group(void* payload) { m_values.m_noninherited.transform.adopt(payload); }
    void adopt_mask_group(void* payload) { m_values.m_noninherited.mask_data.adopt(payload); }
    void adopt_grid_group(void* payload) { m_values.m_noninherited.grid.adopt(payload); }
    void adopt_animation_group(void* payload) { m_values.m_noninherited.animation.adopt(payload); }
    void adopt_svg_reset_group(void* payload) { m_values.m_noninherited.svg_reset.adopt(payload); }
    void adopt_inherited_svg_group(void* payload) { m_values.m_inherited.svg.adopt(payload); }
    void adopt_inherited_list_group(void* payload) { m_values.m_inherited.list.adopt(payload); }
    void adopt_content_group(void* payload) { m_values.m_noninherited.content_data.adopt(payload); }
    void adopt_anchor_group(void* payload) { m_values.m_noninherited.anchor.adopt(payload); }
    void adopt_box_group(void* payload) { m_values.m_noninherited.box.adopt(payload); }
    void adopt_surround_group(void* payload) { m_values.m_noninherited.surround.adopt(payload); }
    void adopt_border_group(void* payload) { m_values.m_noninherited.border.adopt(payload); }
    void adopt_background_group(void* payload) { m_values.m_noninherited.background.adopt(payload); }

    void set_aspect_ratio(AspectRatio aspect_ratio)
    {
        if (m_values.aspect_ratio() == aspect_ratio)
            return;
        m_values.m_noninherited.box.access().aspect_ratio = to_ffi_aspect_ratio(aspect_ratio);
    }
    void set_anchor_names(Vector<Utf16FlyString> value)
    {
        if (m_values.m_noninherited.anchor->anchor_names == value)
            return;
        m_values.m_noninherited.anchor.access().anchor_names = move(value);
    }
    void set_anchor_scope(AnchorScopeData value)
    {
        if (m_values.m_noninherited.anchor->anchor_scope == value)
            return;
        m_values.m_noninherited.anchor.access().anchor_scope = move(value);
    }
    void set_animation_names(Vector<ComputedAnimationName> value)
    {
        if (m_values.m_noninherited.animation->animation_names == value)
            return;
        m_values.m_noninherited.animation.access().animation_names = move(value);
    }
    void set_animation_compositions(Vector<AnimationComposition> value)
    {
        if (m_values.m_noninherited.animation->animation_compositions == value)
            return;
        m_values.m_noninherited.animation.access().animation_compositions = move(value);
    }
    void set_animation_delays(Vector<Time> value)
    {
        if (m_values.m_noninherited.animation->animation_delays == value)
            return;
        m_values.m_noninherited.animation.access().animation_delays = move(value);
    }
    void set_animation_directions(Vector<AnimationDirection> value)
    {
        if (m_values.m_noninherited.animation->animation_directions == value)
            return;
        m_values.m_noninherited.animation.access().animation_directions = move(value);
    }
    void set_animation_durations(Vector<Optional<Time>> value)
    {
        if (m_values.m_noninherited.animation->animation_durations == value)
            return;
        m_values.m_noninherited.animation.access().animation_durations = move(value);
    }
    void set_animation_fill_modes(Vector<AnimationFillMode> value)
    {
        if (m_values.m_noninherited.animation->animation_fill_modes == value)
            return;
        m_values.m_noninherited.animation.access().animation_fill_modes = move(value);
    }
    void set_animation_iteration_counts(Vector<double> value)
    {
        if (m_values.m_noninherited.animation->animation_iteration_counts == value)
            return;
        m_values.m_noninherited.animation.access().animation_iteration_counts = move(value);
    }
    void set_animation_play_states(Vector<AnimationPlayState> value)
    {
        if (m_values.m_noninherited.animation->animation_play_states == value)
            return;
        m_values.m_noninherited.animation.access().animation_play_states = move(value);
    }
    void set_animation_timelines(Vector<AnimationTimelineData> value)
    {
        if (m_values.m_noninherited.animation->animation_timelines == value)
            return;
        m_values.m_noninherited.animation.access().animation_timelines = move(value);
    }
    void set_animation_timing_functions(Vector<EasingFunction> value)
    {
        if (m_values.m_noninherited.animation->animation_timing_functions == value)
            return;
        m_values.m_noninherited.animation.access().animation_timing_functions = move(value);
    }
    void set_animation_timing_function_style_values(StyleValueVector value)
    {
        if (m_values.m_noninherited.animation->animation_timing_function_style_values == value)
            return;
        m_values.m_noninherited.animation.access().animation_timing_function_style_values = move(value);
    }
    void set_caret_color(ColorOrAuto caret_color)
    {
        if (m_values.m_inherited.ui->caret_color == caret_color)
            return;
        m_values.m_inherited.ui.access().caret_color = move(caret_color);
    }
    void set_font_list(NonnullRefPtr<Gfx::FontCascadeList const> font_list)
    {
        // NB: Compare by value, not pointer: the font cascade list is rebuilt per element,
        //     so pointer comparison would defeat sharing of the font group.
        if (m_values.m_inherited.font->font_list && m_values.m_inherited.font->font_list->equals(*font_list))
            return;
        auto& font = m_values.m_inherited.font.access();
        auto const& first_available_font = font_list->font_for_code_point(' ');
        auto const metrics = first_available_font.pixel_metrics();
        font.first_available_font = &first_available_font;
        font.font_ascent = metrics.ascent;
        font.font_descent = metrics.descent;
        font.font_x_height = metrics.x_height;
        font.font_list = move(font_list);
    }
    void set_font_families(Vector<ComputedFontFamily> value)
    {
        if (m_values.m_inherited.font->font_families == value)
            return;
        m_values.m_inherited.font.access().font_families = move(value);
    }
    void set_font_size(CSSPixels font_size)
    {
        if (m_values.m_inherited.font->font_size == font_size)
            return;
        m_values.m_inherited.font.access().font_size = font_size;
    }
    void set_font_weight(double font_weight)
    {
        if (m_values.m_inherited.font->font_weight == font_weight)
            return;
        m_values.m_inherited.font.access().font_weight = font_weight;
    }
    void set_font_width(Percentage font_width)
    {
        if (m_values.m_inherited.font->font_width == font_width)
            return;
        m_values.m_inherited.font.access().font_width = font_width;
    }
    void set_font_style(ComputedFontStyle font_style)
    {
        if (m_values.m_inherited.font->font_style == font_style)
            return;
        m_values.m_inherited.font.access().font_style = move(font_style);
    }
    void set_font_optical_sizing(FontOpticalSizing font_optical_sizing)
    {
        if (m_values.m_inherited.font->font_optical_sizing == font_optical_sizing)
            return;
        m_values.m_inherited.font.access().font_optical_sizing = font_optical_sizing;
    }
    void set_font_feature_data(FontFeatureData font_feature_data)
    {
        if (m_values.m_inherited.font->font_feature_data == font_feature_data)
            return;
        m_values.m_inherited.font.access().font_feature_data = move(font_feature_data);
    }
    void set_font_language_override(Optional<Utf16FlyString> font_language_override)
    {
        if (m_values.m_inherited.font->font_language_override == font_language_override)
            return;
        m_values.m_inherited.font.access().font_language_override = move(font_language_override);
    }
    void set_font_variation_settings(HashMap<Utf16FlyString, double> value)
    {
        auto settings_equal = [&] {
            auto const& current = m_values.m_inherited.font->font_variation_settings;
            if (current.size() != value.size())
                return false;
            for (auto const& entry : value) {
                auto it = current.find(entry.key);
                if (it == current.end() || it->value != entry.value)
                    return false;
            }
            return true;
        };
        if (settings_equal())
            return;
        m_values.m_inherited.font.access().font_variation_settings = move(value);
    }
    void set_line_height(LineHeightData line_height, CSSPixels used_value)
    {
        auto const& font = *m_values.m_inherited.font;
        if (font.line_height == line_height && font.line_height_used == used_value)
            return;
        auto& mutable_font = m_values.m_inherited.font.access();
        mutable_font.line_height_used = used_value;
        mutable_font.line_height = move(line_height);
    }
    void set_border_spacing_horizontal(CSSPixels border_spacing_horizontal)
    {
        if (m_values.m_inherited.table->border_spacing_horizontal == border_spacing_horizontal.raw_value())
            return;
        m_values.m_inherited.table.access().border_spacing_horizontal = border_spacing_horizontal.raw_value();
    }
    void set_border_spacing_vertical(CSSPixels border_spacing_vertical)
    {
        if (m_values.m_inherited.table->border_spacing_vertical == border_spacing_vertical.raw_value())
            return;
        m_values.m_inherited.table.access().border_spacing_vertical = border_spacing_vertical.raw_value();
    }
    void set_caption_side(CaptionSide caption_side)
    {
        if (m_values.m_inherited.table->caption_side == to_underlying(caption_side))
            return;
        m_values.m_inherited.table.access().caption_side = to_underlying(caption_side);
    }
    void set_color(Color color)
    {
        if (m_values.m_inherited.text->color == color)
            return;
        m_values.m_inherited.text.access().color = color;
    }
    void set_color_style_value(StyleValue const* value)
    {
        if (m_values.m_inherited.text->color_style_value.data() == (value ? value->rust_style_value_data() : nullptr))
            return;
        m_values.m_inherited.text.access().color_style_value = retain_style_value_data(value);
    }
    void set_color_interpolation(ColorInterpolation color_interpolation)
    {
        if (m_values.m_inherited.svg->color_interpolation == color_interpolation)
            return;
        m_values.m_inherited.svg.access().color_interpolation = color_interpolation;
    }
    void set_color_interpolation_filters(ColorInterpolation color_interpolation_filters)
    {
        if (m_values.m_inherited.svg->color_interpolation_filters == color_interpolation_filters)
            return;
        m_values.m_inherited.svg.access().color_interpolation_filters = color_interpolation_filters;
    }
    void set_color_scheme(PreferredColorScheme color_scheme)
    {
        if (m_values.m_inherited.ui->color_scheme == color_scheme)
            return;
        m_values.m_inherited.ui.access().color_scheme = color_scheme;
    }
    void set_color_schemes(Vector<Utf16FlyString> color_schemes, bool only)
    {
        if (m_values.m_inherited.ui->color_schemes == color_schemes && m_values.m_inherited.ui->color_scheme_only == only)
            return;
        auto& ui = m_values.m_inherited.ui.access();
        ui.color_schemes = move(color_schemes);
        ui.color_scheme_only = only;
    }
    void set_clip(Clip const& clip)
    {
        if (m_values.m_noninherited.effects->clip == clip)
            return;
        m_values.m_noninherited.effects.access().clip = clip;
    }
    void set_content(ContentData const& content)
    {
        if (m_values.m_noninherited.content_data->content == content)
            return;
        m_values.m_noninherited.content_data.access().content = content;
    }
    void set_computed_content(ComputedContentData content)
    {
        if (m_values.m_noninherited.content_data->computed_content == content)
            return;
        m_values.m_noninherited.content_data.access().computed_content = move(content);
    }
    void set_content_visibility(ContentVisibility content_visibility)
    {
        if (m_values.m_inherited.box->content_visibility == to_underlying(content_visibility))
            return;
        m_values.m_inherited.box.access().content_visibility = to_underlying(content_visibility);
    }
    void set_cursor(Vector<CursorData> cursor)
    {
        if (m_values.m_inherited.ui->cursor == cursor)
            return;
        m_values.m_inherited.ui.access().cursor = move(cursor);
    }
    void set_image_rendering(ImageRendering value)
    {
        if (m_values.m_inherited.box->image_rendering == to_underlying(value))
            return;
        m_values.m_inherited.box.access().image_rendering = to_underlying(value);
    }
    void set_pointer_events(PointerEvents value)
    {
        if (m_values.m_inherited.ui->pointer_events == value)
            return;
        m_values.m_inherited.ui.access().pointer_events = value;
    }
    void set_background_color(Color color)
    {
        if (m_values.m_noninherited.background->background_color == color)
            return;
        m_values.m_noninherited.background.access().background_color = color;
    }
    void set_background_color_style_value(StyleValue const& value)
    {
        if (m_values.m_noninherited.background->background_color_style_value.data() == value.rust_style_value_data())
            return;
        m_values.m_noninherited.background.access().background_color_style_value = retain_style_value_data(&value);
    }
    void set_background_color_clip(BackgroundBox box)
    {
        if (m_values.m_noninherited.background->background_color_clip == box)
            return;
        m_values.m_noninherited.background.access().background_color_clip = box;
    }
    void set_background_layers(Vector<BackgroundLayerData>&& layers)
    {
        if (m_values.m_noninherited.background->background_layers == layers)
            return;
        m_values.m_noninherited.background.access().background_layers = move(layers);
    }
    void set_mask_layers(Vector<BackgroundLayerData>&& layers)
    {
        if (m_values.m_noninherited.mask_data->mask_layers == layers)
            return;
        m_values.m_noninherited.mask_data.access().mask_layers = move(layers);
    }
    void set_mask_positions(Vector<Position> positions)
    {
        if (m_values.m_noninherited.mask_data->mask_positions == positions)
            return;
        m_values.m_noninherited.mask_data.access().mask_positions = move(positions);
    }
    void set_border_image(BorderImageData border_image)
    {
        if (m_values.m_noninherited.border->border_image == border_image)
            return;
        m_values.m_noninherited.border.access().border_image = move(border_image);
    }
    void set_float(Float value)
    {
        if (m_values.m_noninherited.box->float_ == to_underlying(value))
            return;
        m_values.m_noninherited.box.access().float_ = to_underlying(value);
    }
    void set_clear(Clear value)
    {
        if (m_values.m_noninherited.box->clear == to_underlying(value))
            return;
        m_values.m_noninherited.box.access().clear = to_underlying(value);
    }
    void set_z_index(Optional<int> value)
    {
        if (m_values.z_index() == value)
            return;
        auto& box = m_values.m_noninherited.box.access();
        box.has_z_index = value.has_value();
        box.z_index = value.value_or(0);
    }
    void set_tab_size(Variant<CSSPixels, double> value)
    {
        bool const is_number = value.has<double>();
        CSSPixels const length = is_number ? CSSPixels(0) : value.get<CSSPixels>();
        double const number = is_number ? value.get<double>() : 0;
        auto const& text = *m_values.m_inherited.text;
        if (text.tab_size_is_number == is_number && text.tab_size_length == length && text.tab_size_number == number)
            return;
        auto& mutable_text = m_values.m_inherited.text.access();
        mutable_text.tab_size_is_number = is_number;
        mutable_text.tab_size_length = length;
        mutable_text.tab_size_number = number;
    }
    void set_text_align(TextAlign text_align)
    {
        if (m_values.m_inherited.text->text_align == text_align)
            return;
        m_values.m_inherited.text.access().text_align = text_align;
    }
    void set_text_justify(TextJustify text_justify)
    {
        if (m_values.m_inherited.text->text_justify == text_justify)
            return;
        m_values.m_inherited.text.access().text_justify = text_justify;
    }
    void set_text_decoration_line(Vector<TextDecorationLine> value)
    {
        if (m_values.m_noninherited.text_reset->text_decoration_line == value)
            return;
        m_values.m_noninherited.text_reset.access().text_decoration_line = move(value);
    }
    void set_text_decoration_thickness(TextDecorationThickness value)
    {
        if (m_values.m_noninherited.text_reset->text_decoration_thickness == value)
            return;
        m_values.m_noninherited.text_reset.access().text_decoration_thickness = move(value);
    }
    void set_text_decoration_skip_ink(TextDecorationSkipInk value)
    {
        if (m_values.m_inherited.text->text_decoration_skip_ink == value)
            return;
        m_values.m_inherited.text.access().text_decoration_skip_ink = value;
    }
    void set_text_decoration_style(TextDecorationStyle value)
    {
        if (m_values.m_noninherited.text_reset->text_decoration_style == value)
            return;
        m_values.m_noninherited.text_reset.access().text_decoration_style = value;
    }
    void set_text_decoration_color(Color value)
    {
        if (m_values.m_noninherited.text_reset->text_decoration_color == value)
            return;
        m_values.m_noninherited.text_reset.access().text_decoration_color = value;
    }
    void set_text_transform(TextTransform value)
    {
        if (m_values.m_inherited.text->text_transform == value)
            return;
        m_values.m_inherited.text.access().text_transform = value;
    }
    void set_text_shadow(Vector<ShadowData>&& value)
    {
        if (m_values.m_inherited.text->text_shadow == value)
            return;
        m_values.m_inherited.text.access().text_shadow = move(value);
    }
    void set_text_indent(TextIndentData value)
    {
        if (m_values.m_inherited.text->text_indent == value)
            return;
        m_values.m_inherited.text.access().text_indent = move(value);
    }
    void set_text_wrap_mode(TextWrapMode value)
    {
        if (m_values.m_inherited.text->text_wrap_mode == value)
            return;
        m_values.m_inherited.text.access().text_wrap_mode = value;
    }
    void set_text_wrap_style(TextWrapStyle value)
    {
        if (m_values.m_inherited.text->text_wrap_style == value)
            return;
        m_values.m_inherited.text.access().text_wrap_style = value;
    }
    void set_text_underline_offset(TextUnderlineOffset value)
    {
        if (m_values.m_inherited.text->text_underline_offset == value)
            return;
        m_values.m_inherited.text.access().text_underline_offset = move(value);
    }
    void set_text_underline_position(TextUnderlinePosition value)
    {
        if (m_values.m_inherited.text->text_underline_position == value)
            return;
        m_values.m_inherited.text.access().text_underline_position = value;
    }
    void set_webkit_text_fill_color(Color value, bool is_current_color)
    {
        if (m_values.m_inherited.text->webkit_text_fill_color == value && m_values.m_inherited.text->webkit_text_fill_color_is_current_color == is_current_color)
            return;
        auto& text = m_values.m_inherited.text.access();
        text.webkit_text_fill_color = value;
        text.webkit_text_fill_color_is_current_color = is_current_color;
    }
    void set_position(Positioning position)
    {
        if (m_values.m_noninherited.box->position == to_underlying(position))
            return;
        m_values.m_noninherited.box.access().position = to_underlying(position);
    }
    // The surround payload carries a synchronized copy of the position-anchor
    // name for the layout engine's anchor lookup; the zero raw means no name.
    void set_position_anchor(PositionAnchor value)
    {
        if (m_values.m_noninherited.anchor->position_anchor == value)
            return;
        auto const current_name_raw = m_values.m_noninherited.surround->position_anchor_name.raw;
        bool const surround_name_already_matches = value.name.has_value()
            ? current_name_raw != 0 && Utf16FlyString::from_raw(current_name_raw) == *value.name
            : current_name_raw == 0;
        if (!surround_name_already_matches) {
            auto& position_anchor_name = m_values.m_noninherited.surround.access().position_anchor_name;
            if (position_anchor_name.raw)
                Utf16FlyString::unref_raw(exchange(position_anchor_name.raw, 0));
            if (value.name.has_value())
                position_anchor_name.raw = value.name->to_raw_leaked();
        }
        m_values.m_noninherited.anchor.access().position_anchor = move(value);
    }
    void set_position_area(PositionAreaData value)
    {
        if (m_values.m_noninherited.anchor->position_area == value)
            return;
        m_values.m_noninherited.anchor.access().position_area = move(value);
    }
    void set_position_try_fallbacks(Vector<PositionTryFallbackData> value)
    {
        if (m_values.m_noninherited.anchor->position_try_fallbacks == value)
            return;
        m_values.m_noninherited.anchor.access().position_try_fallbacks = move(value);
    }
    void set_position_try_order(Optional<TryOrder> value)
    {
        if (m_values.m_noninherited.anchor->position_try_order == value)
            return;
        m_values.m_noninherited.anchor.access().position_try_order = value;
    }
    void set_position_visibility(PositionVisibilityData value)
    {
        if (m_values.m_noninherited.anchor->position_visibility == value)
            return;
        m_values.m_noninherited.anchor.access().position_visibility = value;
    }
    void set_scroll_timeline_names(Vector<Optional<Utf16FlyString>> value)
    {
        if (m_values.m_noninherited.animation->scroll_timeline_names == value)
            return;
        m_values.m_noninherited.animation.access().scroll_timeline_names = move(value);
    }
    void set_scroll_timeline_axes(Vector<Axis> value)
    {
        if (m_values.m_noninherited.animation->scroll_timeline_axes == value)
            return;
        m_values.m_noninherited.animation.access().scroll_timeline_axes = move(value);
    }
    void set_timeline_scope(TimelineScopeData value)
    {
        if (m_values.m_noninherited.animation->timeline_scope == value)
            return;
        m_values.m_noninherited.animation.access().timeline_scope = move(value);
    }
    void set_view_timeline_names(Vector<Optional<Utf16FlyString>> value)
    {
        if (m_values.m_noninherited.animation->view_timeline_names == value)
            return;
        m_values.m_noninherited.animation.access().view_timeline_names = move(value);
    }
    void set_view_timeline_axes(Vector<Axis> value)
    {
        if (m_values.m_noninherited.animation->view_timeline_axes == value)
            return;
        m_values.m_noninherited.animation.access().view_timeline_axes = move(value);
    }
    void set_view_timeline_insets(Vector<ViewTimelineInsetData> value)
    {
        if (m_values.m_noninherited.animation->view_timeline_insets == value)
            return;
        m_values.m_noninherited.animation.access().view_timeline_insets = move(value);
    }
    void set_transition_properties(Vector<Optional<Utf16FlyString>> value)
    {
        if (m_values.m_noninherited.animation->transition_properties == value)
            return;
        m_values.m_noninherited.animation.access().transition_properties = move(value);
    }
    void set_transition_durations(Vector<Time> value)
    {
        if (m_values.m_noninherited.animation->transition_durations == value)
            return;
        m_values.m_noninherited.animation.access().transition_durations = move(value);
    }
    void set_transition_timing_functions(Vector<EasingFunction> value)
    {
        if (m_values.m_noninherited.animation->transition_timing_functions == value)
            return;
        m_values.m_noninherited.animation.access().transition_timing_functions = move(value);
    }
    void set_transition_timing_function_style_values(StyleValueVector value)
    {
        if (m_values.m_noninherited.animation->transition_timing_function_style_values == value)
            return;
        m_values.m_noninherited.animation.access().transition_timing_function_style_values = move(value);
    }
    void set_transition_delays(Vector<Time> value)
    {
        if (m_values.m_noninherited.animation->transition_delays == value)
            return;
        m_values.m_noninherited.animation.access().transition_delays = move(value);
    }
    void set_transition_behaviors(Vector<TransitionBehavior> value)
    {
        if (m_values.m_noninherited.animation->transition_behaviors == value)
            return;
        m_values.m_noninherited.animation.access().transition_behaviors = move(value);
    }
    void set_white_space_collapse(WhiteSpaceCollapse value)
    {
        if (m_values.m_inherited.text->white_space_collapse == value)
            return;
        m_values.m_inherited.text.access().white_space_collapse = value;
    }
    void set_white_space_trim(WhiteSpaceTrimData value)
    {
        if (m_values.m_noninherited.text_reset->white_space_trim == value)
            return;
        m_values.m_noninherited.text_reset.access().white_space_trim = value;
    }
    void set_word_spacing(CSSPixels value)
    {
        if (m_values.m_inherited.text->word_spacing == value)
            return;
        m_values.m_inherited.text.access().word_spacing = value;
    }
    void set_word_spacing_style_value(StyleValue const& value)
    {
        if (m_values.m_inherited.text->word_spacing_style_value.data() == value.rust_style_value_data())
            return;
        m_values.m_inherited.text.access().word_spacing_style_value = retain_style_value_data(&value);
    }
    void set_letter_spacing_style_value(StyleValue const& value)
    {
        if (m_values.m_inherited.text->letter_spacing_style_value.data() == value.rust_style_value_data())
            return;
        m_values.m_inherited.text.access().letter_spacing_style_value = retain_style_value_data(&value);
    }
    void set_word_break(WordBreak value)
    {
        if (m_values.m_inherited.text->word_break == value)
            return;
        m_values.m_inherited.text.access().word_break = value;
    }
    void set_overflow_wrap(OverflowWrap value)
    {
        if (m_values.m_inherited.text->overflow_wrap == value)
            return;
        m_values.m_inherited.text.access().overflow_wrap = value;
    }
    void set_orphans(u64 value)
    {
        if (m_values.m_inherited.text->orphans == value)
            return;
        m_values.m_inherited.text.access().orphans = value;
    }
    void set_widows(u64 value)
    {
        if (m_values.m_inherited.text->widows == value)
            return;
        m_values.m_inherited.text.access().widows = value;
    }
    void set_font_variant_emoji(FontVariantEmoji value)
    {
        if (m_values.m_inherited.font->font_variant_emoji == value)
            return;
        m_values.m_inherited.font.access().font_variant_emoji = value;
    }
    void set_letter_spacing(CSSPixels value)
    {
        if (m_values.m_inherited.text->letter_spacing == value)
            return;
        m_values.m_inherited.text.access().letter_spacing = value;
    }
    void set_width(Size value) { set_size(&ComputedValuesFFI::SizingValues::width, move(value)); }
    void set_min_width(Size value) { set_size(&ComputedValuesFFI::SizingValues::min_width, move(value)); }
    void set_max_width(Size value) { set_size(&ComputedValuesFFI::SizingValues::max_width, move(value)); }
    void set_height(Size value) { set_size(&ComputedValuesFFI::SizingValues::height, move(value)); }
    void set_min_height(Size value) { set_size(&ComputedValuesFFI::SizingValues::min_height, move(value)); }
    void set_max_height(Size value) { set_size(&ComputedValuesFFI::SizingValues::max_height, move(value)); }
    void set_inset(LengthBox const& inset)
    {
        if (m_values.inset() == inset)
            return;
        set_length_box(m_values.m_noninherited.surround.access().inset, inset);
    }
    void set_margin(LengthBox const& margin)
    {
        if (m_values.margin() == margin)
            return;
        set_length_box(m_values.m_noninherited.surround.access().margin, margin);
    }
    void set_scroll_margin(LengthBox value)
    {
        if (m_values.m_noninherited.misc->scroll_margin == value)
            return;
        m_values.m_noninherited.misc.access().scroll_margin = value;
    }
    void set_scroll_padding(LengthBox value)
    {
        if (m_values.m_noninherited.misc->scroll_padding == value)
            return;
        m_values.m_noninherited.misc.access().scroll_padding = value;
    }
    void set_overflow_clip_margin(OverflowClipMarginData overflow_clip_margin)
    {
        if (m_values.m_noninherited.misc->overflow_clip_margin == overflow_clip_margin)
            return;
        m_values.m_noninherited.misc.access().overflow_clip_margin = overflow_clip_margin;
    }
    void set_overflow_x(Overflow value)
    {
        if (m_values.m_noninherited.box->overflow_x == to_underlying(value))
            return;
        m_values.m_noninherited.box.access().overflow_x = to_underlying(value);
    }
    void set_overflow_y(Overflow value)
    {
        if (m_values.m_noninherited.box->overflow_y == to_underlying(value))
            return;
        m_values.m_noninherited.box.access().overflow_y = to_underlying(value);
    }
    void set_list_style_type(ListStyleType value)
    {
        if (m_values.m_inherited.list->list_style_type == value)
            return;
        m_values.m_inherited.list.access().list_style_type = move(value);
    }
    void set_list_style_position(ListStylePosition value)
    {
        if (m_values.m_inherited.list->list_style_position == value)
            return;
        m_values.m_inherited.list.access().list_style_position = value;
    }
    void set_list_style_image(RefPtr<AbstractImageStyleValue const> value)
    {
        if (m_values.m_inherited.list->list_style_image == value)
            return;
        m_values.m_inherited.list.access().list_style_image = move(value);
    }
    void set_display(Display value)
    {
        if (m_values.display() == value)
            return;
        m_values.m_noninherited.box.access().display = to_ffi_display(value);
    }
    void set_display_before_box_type_transformation(Display value)
    {
        if (m_values.display_before_box_type_transformation() == value)
            return;
        m_values.m_noninherited.box.access().display_before_box_type_transformation = to_ffi_display(value);
    }
    void set_backdrop_filter(Filter const& backdrop_filter)
    {
        if (m_values.m_noninherited.effects->backdrop_filter == backdrop_filter)
            return;
        m_values.m_noninherited.effects.access().backdrop_filter = backdrop_filter;
    }
    void set_filter(Filter const& filter)
    {
        if (m_values.m_noninherited.effects->filter == filter)
            return;
        m_values.m_noninherited.effects.access().filter = filter;
    }
    void set_border_bottom_left_radius(BorderRadiusData value)
    {
        if (value.is_initial() && !m_values.m_noninherited.border->has_noninitial_border_radii)
            return;
        if (m_values.m_noninherited.border->border_bottom_left_radius == value && m_values.m_noninherited.border->has_noninitial_border_radii)
            return;
        auto& border = m_values.m_noninherited.border.access();
        border.has_noninitial_border_radii = true;
        border.border_bottom_left_radius = move(value);
    }
    void set_border_bottom_right_radius(BorderRadiusData value)
    {
        if (value.is_initial() && !m_values.m_noninherited.border->has_noninitial_border_radii)
            return;
        if (m_values.m_noninherited.border->border_bottom_right_radius == value && m_values.m_noninherited.border->has_noninitial_border_radii)
            return;
        auto& border = m_values.m_noninherited.border.access();
        border.has_noninitial_border_radii = true;
        border.border_bottom_right_radius = move(value);
    }
    void set_border_top_left_radius(BorderRadiusData value)
    {
        if (value.is_initial() && !m_values.m_noninherited.border->has_noninitial_border_radii)
            return;
        if (m_values.m_noninherited.border->border_top_left_radius == value && m_values.m_noninherited.border->has_noninitial_border_radii)
            return;
        auto& border = m_values.m_noninherited.border.access();
        border.has_noninitial_border_radii = true;
        border.border_top_left_radius = move(value);
    }
    void set_border_top_right_radius(BorderRadiusData value)
    {
        if (value.is_initial() && !m_values.m_noninherited.border->has_noninitial_border_radii)
            return;
        if (m_values.m_noninherited.border->border_top_right_radius == value && m_values.m_noninherited.border->has_noninitial_border_radii)
            return;
        auto& border = m_values.m_noninherited.border.access();
        border.has_noninitial_border_radii = true;
        border.border_top_right_radius = move(value);
    }
    void set_corner_bottom_left_shape(double value)
    {
        if (m_values.m_noninherited.border->corner_bottom_left_shape == value)
            return;
        m_values.m_noninherited.border.access().corner_bottom_left_shape = value;
    }
    void set_corner_bottom_right_shape(double value)
    {
        if (m_values.m_noninherited.border->corner_bottom_right_shape == value)
            return;
        m_values.m_noninherited.border.access().corner_bottom_right_shape = value;
    }
    void set_corner_top_left_shape(double value)
    {
        if (m_values.m_noninherited.border->corner_top_left_shape == value)
            return;
        m_values.m_noninherited.border.access().corner_top_left_shape = value;
    }
    void set_corner_top_right_shape(double value)
    {
        if (m_values.m_noninherited.border->corner_top_right_shape == value)
            return;
        m_values.m_noninherited.border.access().corner_top_right_shape = value;
    }
    void set_border_left(BorderData value)
    {
        if (m_values.m_noninherited.border->border_left == value)
            return;
        m_values.m_noninherited.border.access().border_left = value;
    }
    void set_border_top(BorderData value)
    {
        if (m_values.m_noninherited.border->border_top == value)
            return;
        m_values.m_noninherited.border.access().border_top = value;
    }
    void set_border_right(BorderData value)
    {
        if (m_values.m_noninherited.border->border_right == value)
            return;
        m_values.m_noninherited.border.access().border_right = value;
    }
    void set_border_bottom(BorderData value)
    {
        if (m_values.m_noninherited.border->border_bottom == value)
            return;
        m_values.m_noninherited.border.access().border_bottom = value;
    }
    void set_border_left_color_style_value(StyleValue const& value)
    {
        if (m_values.m_noninherited.border->border_left_color_style_value.data() == value.rust_style_value_data())
            return;
        m_values.m_noninherited.border.access().border_left_color_style_value = retain_style_value_data(&value);
    }
    void set_border_top_color_style_value(StyleValue const& value)
    {
        if (m_values.m_noninherited.border->border_top_color_style_value.data() == value.rust_style_value_data())
            return;
        m_values.m_noninherited.border.access().border_top_color_style_value = retain_style_value_data(&value);
    }
    void set_border_right_color_style_value(StyleValue const& value)
    {
        if (m_values.m_noninherited.border->border_right_color_style_value.data() == value.rust_style_value_data())
            return;
        m_values.m_noninherited.border.access().border_right_color_style_value = retain_style_value_data(&value);
    }
    void set_border_bottom_color_style_value(StyleValue const& value)
    {
        if (m_values.m_noninherited.border->border_bottom_color_style_value.data() == value.rust_style_value_data())
            return;
        m_values.m_noninherited.border.access().border_bottom_color_style_value = retain_style_value_data(&value);
    }
    void set_border_left_computed_width(CSSPixels value)
    {
        if (m_values.m_noninherited.border->border_left_computed_width == value)
            return;
        m_values.m_noninherited.border.access().border_left_computed_width = value;
    }
    void set_border_top_computed_width(CSSPixels value)
    {
        if (m_values.m_noninherited.border->border_top_computed_width == value)
            return;
        m_values.m_noninherited.border.access().border_top_computed_width = value;
    }
    void set_border_right_computed_width(CSSPixels value)
    {
        if (m_values.m_noninherited.border->border_right_computed_width == value)
            return;
        m_values.m_noninherited.border.access().border_right_computed_width = value;
    }
    void set_border_bottom_computed_width(CSSPixels value)
    {
        if (m_values.m_noninherited.border->border_bottom_computed_width == value)
            return;
        m_values.m_noninherited.border.access().border_bottom_computed_width = value;
    }
    void set_flex_direction(FlexDirection value)
    {
        if (m_values.flex_direction() == value)
            return;
        m_values.m_noninherited.alignment.access().flex_direction = to_underlying(value);
    }
    void set_flex_wrap(FlexWrap value)
    {
        if (m_values.flex_wrap() == value)
            return;
        m_values.m_noninherited.alignment.access().flex_wrap = to_underlying(value);
    }
    void set_flex_grow(double value)
    {
        if (m_values.m_noninherited.alignment->flex_grow == value)
            return;
        m_values.m_noninherited.alignment.access().flex_grow = value;
    }
    void set_flex_shrink(double value)
    {
        if (m_values.m_noninherited.alignment->flex_shrink == value)
            return;
        m_values.m_noninherited.alignment.access().flex_shrink = value;
    }
    void set_order(i32 value)
    {
        if (m_values.m_noninherited.alignment->order == value)
            return;
        m_values.m_noninherited.alignment.access().order = value;
    }
    void set_accent_color(ColorOrAuto value)
    {
        if (m_values.m_inherited.ui->accent_color == value)
            return;
        m_values.m_inherited.ui.access().accent_color = move(value);
    }
    void set_align_content(AlignContent value)
    {
        if (m_values.align_content() == value)
            return;
        m_values.m_noninherited.alignment.access().align_content = to_underlying(value);
    }
    void set_align_items(AlignItems value)
    {
        if (m_values.align_items() == value)
            return;
        m_values.m_noninherited.alignment.access().align_items = to_underlying(value);
    }
    void set_align_self(AlignSelf value)
    {
        if (m_values.align_self() == value)
            return;
        m_values.m_noninherited.alignment.access().align_self = to_underlying(value);
    }
    void set_appearance(Appearance value)
    {
        if (m_values.m_noninherited.misc->appearance == value)
            return;
        m_values.m_noninherited.misc.access().appearance = value;
    }
    void set_computed_appearance(Appearance value)
    {
        if (m_values.m_noninherited.misc->computed_appearance == value)
            return;
        m_values.m_noninherited.misc.access().computed_appearance = value;
    }
    void set_opacity(float value)
    {
        if (m_values.m_noninherited.effects->opacity == value)
            return;
        m_values.m_noninherited.effects.access().opacity = value;
    }
    void set_justify_content(JustifyContent value)
    {
        if (m_values.justify_content() == value)
            return;
        m_values.m_noninherited.alignment.access().justify_content = to_underlying(value);
    }
    void set_justify_items(JustifyItems value)
    {
        if (m_values.justify_items() == value)
            return;
        m_values.m_noninherited.alignment.access().justify_items = to_underlying(value);
    }
    void set_justify_self(JustifySelf value)
    {
        if (m_values.justify_self() == value)
            return;
        m_values.m_noninherited.alignment.access().justify_self = to_underlying(value);
    }
    void set_box_shadow(Vector<ShadowData>&& value)
    {
        if (m_values.m_noninherited.effects->box_shadow == value)
            return;
        m_values.m_noninherited.effects.access().box_shadow = move(value);
    }
    void set_rotate(RefPtr<TransformationStyleValue const> value)
    {
        if (m_values.m_noninherited.transform->rotate == value)
            return;
        m_values.m_noninherited.transform.access().rotate = move(value);
    }
    void set_scale(RefPtr<TransformationStyleValue const> value)
    {
        if (m_values.m_noninherited.transform->scale == value)
            return;
        m_values.m_noninherited.transform.access().scale = move(value);
    }
    void set_perspective(Optional<CSSPixels> value)
    {
        if (m_values.m_noninherited.transform->perspective == value)
            return;
        m_values.m_noninherited.transform.access().perspective = move(value);
    }
    void set_perspective_origin(Position value)
    {
        if (m_values.m_noninherited.transform->perspective_origin == value)
            return;
        m_values.m_noninherited.transform.access().perspective_origin = move(value);
    }
    void set_transformations(Vector<NonnullRefPtr<TransformationStyleValue const>> value)
    {
        if (m_values.m_noninherited.transform->transformations == value)
            return;
        m_values.m_noninherited.transform.access().transformations = move(value);
    }
    void set_transform_box(TransformBox value)
    {
        if (m_values.m_noninherited.transform->transform_box == value)
            return;
        m_values.m_noninherited.transform.access().transform_box = value;
    }
    void set_transform_origin(TransformOrigin value)
    {
        if (m_values.m_noninherited.transform->transform_origin == value)
            return;
        m_values.m_noninherited.transform.access().transform_origin = value;
    }
    void set_transform_style(TransformStyle value)
    {
        if (m_values.m_noninherited.transform->transform_style == value)
            return;
        m_values.m_noninherited.transform.access().transform_style = value;
    }
    void set_translate(RefPtr<TransformationStyleValue const> value)
    {
        if (m_values.m_noninherited.transform->translate == value)
            return;
        m_values.m_noninherited.transform.access().translate = move(value);
    }
    void set_box_sizing(BoxSizing value)
    {
        if (m_values.m_noninherited.box->box_sizing == to_underlying(value))
            return;
        m_values.m_noninherited.box.access().box_sizing = to_underlying(value);
    }
    void set_vertical_align(Variant<VerticalAlign, LengthPercentage> value)
    {
        if (m_values.vertical_align() == value)
            return;
        auto& slot = m_values.m_noninherited.box.access().vertical_align;
        StyleValueFFI::rust_style_value_release(static_cast<StyleValueFFI::StyleValueData const*>(slot.value.pointer));
        slot = to_ffi_vertical_align(value);
    }
    void set_visibility(Visibility value)
    {
        if (m_values.m_inherited.box->visibility == to_underlying(value))
            return;
        m_values.m_inherited.box.access().visibility = to_underlying(value);
    }
    void set_grid_auto_columns(GridTrackSizeList value)
    {
        if (m_values.m_noninherited.grid->grid_auto_columns == value)
            return;
        m_values.m_noninherited.grid.access().grid_auto_columns = move(value);
    }
    void set_grid_auto_rows(GridTrackSizeList value)
    {
        if (m_values.m_noninherited.grid->grid_auto_rows == value)
            return;
        m_values.m_noninherited.grid.access().grid_auto_rows = move(value);
    }
    void set_grid_template_columns(GridTrackSizeList value)
    {
        if (m_values.m_noninherited.grid->grid_template_columns == value)
            return;
        m_values.m_noninherited.grid.access().grid_template_columns = move(value);
    }
    void set_grid_template_rows(GridTrackSizeList value)
    {
        if (m_values.m_noninherited.grid->grid_template_rows == value)
            return;
        m_values.m_noninherited.grid.access().grid_template_rows = move(value);
    }
    void set_grid_column_end(GridTrackPlacement value)
    {
        if (m_values.m_noninherited.grid->grid_column_end == value)
            return;
        m_values.m_noninherited.grid.access().grid_column_end = move(value);
    }
    void set_grid_column_start(GridTrackPlacement value)
    {
        if (m_values.m_noninherited.grid->grid_column_start == value)
            return;
        m_values.m_noninherited.grid.access().grid_column_start = move(value);
    }
    void set_grid_row_end(GridTrackPlacement value)
    {
        if (m_values.m_noninherited.grid->grid_row_end == value)
            return;
        m_values.m_noninherited.grid.access().grid_row_end = move(value);
    }
    void set_grid_row_start(GridTrackPlacement value)
    {
        if (m_values.m_noninherited.grid->grid_row_start == value)
            return;
        m_values.m_noninherited.grid.access().grid_row_start = move(value);
    }
    void set_column_span(ColumnSpan column_span)
    {
        if (m_values.m_noninherited.misc->column_span == column_span)
            return;
        m_values.m_noninherited.misc.access().column_span = column_span;
    }
    void set_column_height(Size column_height)
    {
        if (m_values.m_noninherited.misc->column_height == column_height)
            return;
        m_values.m_noninherited.misc.access().column_height = column_height;
    }
    void set_border_collapse(BorderCollapse const border_collapse)
    {
        if (m_values.m_inherited.table->border_collapse == to_underlying(border_collapse))
            return;
        m_values.m_inherited.table.access().border_collapse = to_underlying(border_collapse);
    }
    void set_empty_cells(EmptyCells const empty_cells)
    {
        if (m_values.m_inherited.table->empty_cells == to_underlying(empty_cells))
            return;
        m_values.m_inherited.table.access().empty_cells = to_underlying(empty_cells);
    }
    void set_grid_template_areas(GridTemplateAreas grid_template_areas)
    {
        if (m_values.m_noninherited.grid->grid_template_areas == grid_template_areas)
            return;
        m_values.m_noninherited.grid.access().grid_template_areas = move(grid_template_areas);
    }
    void set_quotes(QuotesData value)
    {
        if (m_values.m_inherited.list->quotes == value)
            return;
        m_values.m_inherited.list.access().quotes = move(value);
    }
    void set_object_fit(ObjectFit value)
    {
        if (m_values.m_noninherited.misc->object_fit == value)
            return;
        m_values.m_noninherited.misc.access().object_fit = value;
    }
    void set_object_position(Position value)
    {
        if (m_values.m_noninherited.misc->object_position == value)
            return;
        m_values.m_noninherited.misc.access().object_position = move(value);
    }
    void set_direction(Direction value)
    {
        if (m_values.m_inherited.box->direction == to_underlying(value))
            return;
        m_values.m_inherited.box.access().direction = to_underlying(value);
    }
    void set_dominant_baseline(Optional<BaselineMetric> value)
    {
        if (m_values.m_inherited.svg->dominant_baseline == value)
            return;
        m_values.m_inherited.svg.access().dominant_baseline = value;
    }
    void set_writing_mode(WritingMode value)
    {
        if (m_values.m_inherited.box->writing_mode == to_underlying(value))
            return;
        m_values.m_inherited.box.access().writing_mode = to_underlying(value);
    }
    void set_user_select(UserSelect value)
    {
        if (m_values.m_noninherited.misc->user_select == value)
            return;
        m_values.m_noninherited.misc.access().user_select = value;
    }
    void set_isolation(Isolation value)
    {
        if (m_values.m_noninherited.effects->isolation == value)
            return;
        m_values.m_noninherited.effects.access().isolation = value;
    }
    void set_contain(Containment value)
    {
        if (m_values.contain() == value)
            return;
        auto& box = m_values.m_noninherited.box.access();
        box.size_containment = value.size_containment;
        box.inline_size_containment = value.inline_size_containment;
        box.layout_containment = value.layout_containment;
        box.style_containment = value.style_containment;
        box.paint_containment = value.paint_containment;
    }
    void set_container_name(Vector<Utf16FlyString> value)
    {
        auto const& stored = m_values.m_noninherited.box->container_name;
        auto stored_names_equal_value = [&] {
            if (stored.length != value.size())
                return false;
            for (size_t i = 0; i < stored.length; ++i) {
                if (Utf16FlyString::from_raw(stored.pointer[i].raw) != value[i])
                    return false;
            }
            return true;
        };
        if (stored_names_equal_value())
            return;
        auto leaked_raws = to_leaked_fly_string_raws(value);
        ComputedValuesFFI::rust_replace_computed_fly_string_list(
            &m_values.m_noninherited.box.access().container_name, leaked_raws.data(), leaked_raws.size());
    }
    void set_container_type(ContainerType value)
    {
        if (m_values.container_type() == value)
            return;
        auto& box = m_values.m_noninherited.box.access();
        box.is_size_container = value.is_size_container;
        box.is_inline_size_container = value.is_inline_size_container;
        box.is_scroll_state_container = value.is_scroll_state_container;
    }
    void set_mix_blend_mode(MixBlendMode value)
    {
        if (m_values.m_noninherited.effects->mix_blend_mode == value)
            return;
        m_values.m_noninherited.effects.access().mix_blend_mode = value;
    }
    void set_view_transition_name(Optional<Utf16FlyString> value)
    {
        if (m_values.m_noninherited.misc->view_transition_name == value)
            return;
        m_values.m_noninherited.misc.access().view_transition_name = move(value);
    }
    void set_touch_action(TouchActionData value)
    {
        if (m_values.m_noninherited.misc->touch_action == value)
            return;
        m_values.m_noninherited.misc.access().touch_action = value;
    }

    void set_fill(Optional<SVGPaint> value)
    {
        if (m_values.m_inherited.svg->fill == value)
            return;
        m_values.m_inherited.svg.access().fill = move(value);
    }
    void set_stroke(Optional<SVGPaint> value)
    {
        if (m_values.m_inherited.svg->stroke == value)
            return;
        m_values.m_inherited.svg.access().stroke = move(value);
    }
    void set_fill_rule(FillRule value)
    {
        if (m_values.m_inherited.svg->fill_rule == value)
            return;
        m_values.m_inherited.svg.access().fill_rule = value;
    }
    void set_fill_opacity(float value)
    {
        if (m_values.m_inherited.svg->fill_opacity == value)
            return;
        m_values.m_inherited.svg.access().fill_opacity = value;
    }
    void set_stroke_dasharray(Vector<Variant<LengthPercentage, float>> value)
    {
        if (m_values.m_inherited.svg->stroke_dasharray == value)
            return;
        m_values.m_inherited.svg.access().stroke_dasharray = move(value);
    }
    void set_stroke_dashoffset(LengthPercentage value)
    {
        if (m_values.m_inherited.svg->stroke_dashoffset == value)
            return;
        m_values.m_inherited.svg.access().stroke_dashoffset = move(value);
    }
    void set_stroke_linecap(StrokeLinecap value)
    {
        if (m_values.m_inherited.svg->stroke_linecap == value)
            return;
        m_values.m_inherited.svg.access().stroke_linecap = value;
    }
    void set_stroke_linejoin(StrokeLinejoin value)
    {
        if (m_values.m_inherited.svg->stroke_linejoin == value)
            return;
        m_values.m_inherited.svg.access().stroke_linejoin = value;
    }
    void set_stroke_miterlimit(double value)
    {
        if (m_values.m_inherited.svg->stroke_miterlimit == value)
            return;
        m_values.m_inherited.svg.access().stroke_miterlimit = value;
    }
    void set_stroke_opacity(float value)
    {
        if (m_values.m_inherited.svg->stroke_opacity == value)
            return;
        m_values.m_inherited.svg.access().stroke_opacity = value;
    }
    void set_stroke_width(LengthPercentage value)
    {
        if (m_values.m_inherited.svg->stroke_width == value)
            return;
        m_values.m_inherited.svg.access().stroke_width = move(value);
    }
    void set_text_anchor(TextAnchor value)
    {
        if (m_values.m_inherited.svg->text_anchor == value)
            return;
        m_values.m_inherited.svg.access().text_anchor = value;
    }
    void set_outline_color(Color value)
    {
        if (m_values.m_noninherited.misc->outline_color == value)
            return;
        m_values.m_noninherited.misc.access().outline_color = value;
    }
    void set_outline_offset(CSSPixels value)
    {
        if (m_values.m_noninherited.misc->outline_offset == value)
            return;
        m_values.m_noninherited.misc.access().outline_offset = move(value);
    }
    void set_outline_offset_style_value(StyleValue const& value)
    {
        if (m_values.m_noninherited.misc->outline_offset_style_value == &value)
            return;
        m_values.m_noninherited.misc.access().outline_offset_style_value = value;
    }
    void set_outline_style(OutlineStyle value)
    {
        if (m_values.m_noninherited.misc->outline_style == value)
            return;
        m_values.m_noninherited.misc.access().outline_style = value;
    }
    void set_outline_width(CSSPixels value)
    {
        if (m_values.m_noninherited.misc->outline_width == value)
            return;
        m_values.m_noninherited.misc.access().outline_width = value;
    }
    void set_mask(MaskReference value)
    {
        if (m_values.m_noninherited.mask_data->mask == value)
            return;
        m_values.m_noninherited.mask_data.access().mask = value;
    }
    void set_mask_type(MaskType value)
    {
        if (m_values.m_noninherited.mask_data->mask_type == value)
            return;
        m_values.m_noninherited.mask_data.access().mask_type = value;
    }
    void set_mask_image(AbstractImageStyleValue const& value)
    {
        if (m_values.m_noninherited.mask_data->mask_image == &value)
            return;
        m_values.m_noninherited.mask_data.access().mask_image = value;
    }
    void set_clip_path(ClipPathReference value)
    {
        if (m_values.m_noninherited.mask_data->clip_path == value)
            return;
        m_values.m_noninherited.mask_data.access().clip_path = move(value);
    }
    void set_clip_rule(ClipRule value)
    {
        if (m_values.m_inherited.svg->clip_rule == value)
            return;
        m_values.m_inherited.svg.access().clip_rule = value;
    }
    void set_paint_order(PaintOrderList value)
    {
        if (m_values.m_inherited.svg->paint_order == value)
            return;
        m_values.m_inherited.svg.access().paint_order = value;
    }
    void set_paint_order_serialization(u8 length, bool is_normal)
    {
        if (m_values.m_inherited.svg->paint_order_serialization_length == length && m_values.m_inherited.svg->paint_order_is_normal == is_normal)
            return;
        auto& svg = m_values.m_inherited.svg.access();
        svg.paint_order_serialization_length = length;
        svg.paint_order_is_normal = is_normal;
    }

    void set_math_shift(MathShift value)
    {
        if (m_values.m_inherited.font->math_shift == value)
            return;
        m_values.m_inherited.font.access().math_shift = value;
    }
    void set_math_style(MathStyle value)
    {
        if (m_values.m_inherited.font->math_style == value)
            return;
        m_values.m_inherited.font.access().math_style = value;
    }
    void set_math_depth(int value)
    {
        if (m_values.m_inherited.font->math_depth == value)
            return;
        m_values.m_inherited.font.access().math_depth = value;
    }

    void set_scroll_behavior(ScrollBehavior value)
    {
        if (m_values.m_noninherited.misc->scroll_behavior == value)
            return;
        m_values.m_noninherited.misc.access().scroll_behavior = value;
    }
    void set_scrollbar_color(ScrollbarColorData value)
    {
        if (m_values.m_inherited.ui->scrollbar_color == value)
            return;
        m_values.m_inherited.ui.access().scrollbar_color = value;
    }
    void set_scrollbar_gutter(ScrollbarGutter value)
    {
        if (m_values.m_noninherited.misc->scrollbar_gutter == value)
            return;
        m_values.m_noninherited.misc.access().scrollbar_gutter = value;
    }
    void set_scrollbar_width(ScrollbarWidth value)
    {
        if (m_values.m_noninherited.misc->scrollbar_width == value)
            return;
        m_values.m_noninherited.misc.access().scrollbar_width = value;
    }
    void set_resize(Resize value)
    {
        if (m_values.m_noninherited.box->resize == to_underlying(value))
            return;
        m_values.m_noninherited.box.access().resize = to_underlying(value);
    }
    void set_shape_image_threshold(double value)
    {
        if (m_values.m_noninherited.misc->shape_image_threshold == value)
            return;
        m_values.m_noninherited.misc.access().shape_image_threshold = value;
    }
    void set_shape_margin(LengthPercentage value)
    {
        if (m_values.m_noninherited.misc->shape_margin == value)
            return;
        m_values.m_noninherited.misc.access().shape_margin = move(value);
    }
    void set_shape_outside(ShapeOutsideData value)
    {
        if (m_values.m_noninherited.misc->shape_outside == value)
            return;
        m_values.m_noninherited.misc.access().shape_outside = move(value);
    }

    void set_counter_increment(Vector<CounterData> value)
    {
        if (m_values.m_noninherited.content_data->counter_increment == value)
            return;
        m_values.m_noninherited.content_data.access().counter_increment = move(value);
    }
    void set_counter_reset(Vector<CounterData> value)
    {
        if (m_values.m_noninherited.content_data->counter_reset == value)
            return;
        m_values.m_noninherited.content_data.access().counter_reset = move(value);
    }
    void set_counter_set(Vector<CounterData> value)
    {
        if (m_values.m_noninherited.content_data->counter_set == value)
            return;
        m_values.m_noninherited.content_data.access().counter_set = move(value);
    }

    void set_will_change(WillChange value)
    {
        if (m_values.m_noninherited.misc->will_change == value)
            return;
        m_values.m_noninherited.misc.access().will_change = move(value);
    }

private:
    static void replace_length_percentage_or_auto(ComputedValuesFFI::ComputedLengthPercentageOrAuto& target, LengthPercentageOrAuto const& replacement)
    {
        StyleValueFFI::rust_style_value_release(static_cast<StyleValueFFI::StyleValueData const*>(target.value.pointer));
        target.is_auto = replacement.is_auto();
        if (replacement.is_auto()) {
            target.value.pointer = nullptr;
            return;
        }
        auto retained = replacement.length_percentage();
        target.value.pointer = retained.leak_data();
    }

    static void set_length_box(ComputedValuesFFI::ComputedLengthBox& target, LengthBox const& replacement)
    {
        replace_length_percentage_or_auto(target.top, replacement.top());
        replace_length_percentage_or_auto(target.right, replacement.right());
        replace_length_percentage_or_auto(target.bottom, replacement.bottom());
        replace_length_percentage_or_auto(target.left, replacement.left());
    }

    void set_size(ComputedValuesFFI::ComputedSize ComputedValuesFFI::SizingValues::* member, Size value)
    {
        if (Size::view(m_values.m_noninherited.sizing.operator->()->*member) == value)
            return;
        Size::replace(m_values.m_noninherited.sizing.access().*member, move(value));
    }

    ComputedValues& m_values;
};

class ComputedValues::Builder {
public:
    Builder()
        : m_values(adopt_ref(*new ComputedValues))
        , m_mutator(*m_values)
    {
    }

    explicit Builder(ComputedValues const& values)
        : Builder()
    {
        m_values->m_inherited = values.m_inherited;
        m_values->m_noninherited = values.m_noninherited;
        m_values->m_property_important = values.m_property_important;
        m_values->m_property_inherited = values.m_property_inherited;
        m_values->m_inheritance_dependent_specified_values = values.m_inheritance_dependent_specified_values;
        m_values->m_raw_cascaded_font_size = values.m_raw_cascaded_font_size;
        m_values->m_base_values = values.m_base_values;
        m_mutator.set_animated_properties(values.m_animated_properties.ptr());
        m_values->m_pseudo_element_styles = values.m_pseudo_element_styles;
        m_values->m_depends_on_viewport_metrics = values.m_depends_on_viewport_metrics;
        m_values->m_font_metrics_depend_on_viewport_metrics = values.m_font_metrics_depend_on_viewport_metrics;
        m_values->m_in_display_none_subtree = values.m_in_display_none_subtree;
    }

    static Builder create_inheriting_from(ComputedValues const& values)
    {
        Builder builder;
        builder.m_values->m_inherited = values.m_inherited;
        return builder;
    }

    Mutator* operator->() { return &m_mutator; }
    Mutator const* operator->() const { return &m_mutator; }

    NonnullRefPtr<ComputedValues const> build() && { return move(m_values); }

private:
    NonnullRefPtr<ComputedValues> m_values;
    Mutator m_mutator;
};

}
