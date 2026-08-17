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
#include <AK/Span.h>
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
#include <LibWeb/CSS/LengthBox.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/Ratio.h>
#include <LibWeb/CSS/ResolvedTransform.h>
#include <LibWeb/CSS/Size.h>
#include <LibWeb/CSS/StyleRecordID.h>
#include <LibWeb/CSS/StyleStructRef.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/BasicShapeStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/CursorStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/Time.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/StyleEngineRustFFI.h>

namespace Web::DOM {

class Document;

}

namespace Web::CSS {

class AnimatedProperties;

class ComputedFilterView {
public:
    explicit ComputedFilterView(ComputedValuesFFI::ComputedFilter const& filter)
        : m_filter(filter)
    {
    }

    bool has_filters() const { return m_filter.filter_list.pointer; }
    bool is_none() const { return !has_filters(); }

    template<typename Callback>
    void for_each_operation(Callback callback) const
    {
        for (size_t index = 0; index < m_filter.operations.length; ++index) {
            auto const& operation = m_filter.operations.pointer[index];
            switch (operation.kind) {
            case to_underlying(FilterStyleValue::Kind::Blur):
                callback(Filter::FilterOperation { Filter::Blur { .resolved_radius = operation.amount } });
                break;
            case to_underlying(FilterStyleValue::Kind::DropShadow):
                callback(Filter::FilterOperation { Filter::DropShadow {
                    .offset_x = CSSPixels::from_raw(operation.shadow_offset_x),
                    .offset_y = CSSPixels::from_raw(operation.shadow_offset_y),
                    .radius = CSSPixels::from_raw(operation.shadow_radius),
                    .color = Color::from_bgra(operation.shadow_color),
                } });
                break;
            case to_underlying(FilterStyleValue::Kind::HueRotate):
                callback(Filter::FilterOperation { Filter::HueRotate { .angle_degrees = operation.amount } });
                break;
            case to_underlying(FilterStyleValue::Kind::Color):
                callback(Filter::FilterOperation { Filter::ColorOperation {
                    .operation = static_cast<Gfx::ColorFilterType>(operation.color_operation),
                    .resolved_amount = operation.amount,
                } });
                break;
            case 4:
                callback(Filter::FilterOperation { Filter::Url { url_fragment(operation.url_value) } });
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        }
    }

    Filter materialize() const
    {
        if (!has_filters())
            return Filter::make_none();
        Vector<Filter::FilterOperation> operations;
        operations.ensure_capacity(m_filter.operations.length);
        for_each_operation([&](auto operation) { operations.unchecked_append(move(operation)); });
        auto filter_list = retained_style_value(m_filter.filter_list);
        RefPtr<StyleValueList const> list = &filter_list->as_value_list();
        return Filter::create_lowered(move(list), move(operations));
    }

private:
    static RefPtr<StyleValue const> retained_style_value(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
    {
        if (!handle.pointer)
            return nullptr;
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
            static_cast<StyleValueFFI::StyleValueData const*>(handle.pointer)));
    }

    static Utf16String url_fragment(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
    {
        auto value = retained_style_value(handle);
        VERIFY(value);
        auto url = value->as_url().url();
        auto const& url_string = url.url();
        if (url_string.is_empty() || !url_string.starts_with('#'))
            return {};
        auto fragment = url_string.substring_from_byte_offset(1);
        if (fragment.is_error())
            return {};
        return Utf16String::from_utf8(fragment.release_value());
    }

    ComputedValuesFFI::ComputedFilter const& m_filter;
};

class ComputedStyleWorkingSet;
class ComputedStyleRecordView;
class StyleComputer;
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
    static Variant<LengthPercentage, NormalGap> column_gap() { return NormalGap {}; }
    static ColumnSpan column_span() { return ColumnSpan::None; }
    static Size column_height() { return Size::make_auto(); }
    static Variant<LengthPercentage, NormalGap> row_gap() { return NormalGap {}; }
    static BorderCollapse border_collapse() { return BorderCollapse::Separate; }
    static EmptyCells empty_cells() { return EmptyCells::Show; }
    static ObjectFit object_fit() { return ObjectFit::Fill; }
    static Position object_position() { return {}; }
    static Color outline_color() { return Color::Black; }
    static CSSPixels outline_offset() { return 0; }
    static OutlineStyle outline_style() { return OutlineStyle::None; }
    static CSSPixels outline_width() { return 3; }
    static QuotesData quotes() { return QuotesData { .type = QuotesData::Type::Auto }; }
    static TransformBox transform_box() { return TransformBox::ViewBox; }
    static TransformStyle transform_style() { return TransformStyle::Flat; }
    static BackfaceVisibility backface_visibility() { return BackfaceVisibility::Visible; }
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

// Every ComputedValues style value group, in vtable registration order:
// G(enumerator, member path, sharing-info name, affects layout).
// A group may be flagged as not affecting layout only when every one of its fields is
// read exclusively at paint or display-list build time: background qualifies (paint;
// resource-observer registration runs in apply_style regardless of layout), mask
// qualifies (paint and hit-testing), and text_reset qualifies (text-decoration resolves
// at display-list build; white-space-trim is unread by layout today, so implementing it
// must revisit the flag). effects stays layout-affecting because filter/backdrop-filter
// establish fixed-positioning containing blocks and re-parent abspos descendants.
#define LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(G)                 \
    G(InheritedTableValues, m_inherited.table, "inheritedTable", true)  \
    G(InheritedListValues, m_inherited.list, "inheritedList", true)     \
    G(InheritedUIValues, m_inherited.ui, "inheritedUI", true)           \
    G(InheritedSVGValues, m_inherited.svg, "inheritedSVG", true)        \
    G(InheritedTextValues, m_inherited.text, "inheritedText", true)     \
    G(InheritedBoxValues, m_inherited.box, "inheritedBox", true)        \
    G(FontValues, m_inherited.font, "font", true)                       \
    G(AnimationValues, m_noninherited.animation, "animation", true)     \
    G(SVGResetValues, m_noninherited.svg_reset, "svgReset", true)       \
    G(GridValues, m_noninherited.grid, "grid", true)                    \
    G(AnchorValues, m_noninherited.anchor, "anchor", true)              \
    G(EffectsValues, m_noninherited.effects, "effects", true)           \
    G(MaskValues, m_noninherited.mask_data, "mask", false)              \
    G(TextResetValues, m_noninherited.text_reset, "textReset", false)   \
    G(ContentValues, m_noninherited.content_data, "content", true)      \
    G(TransformValues, m_noninherited.transform, "transform", true)     \
    G(BackgroundValues, m_noninherited.background, "background", false) \
    G(BorderValues, m_noninherited.border, "border", true)              \
    G(AlignmentValues, m_noninherited.alignment, "alignment", true)     \
    G(MiscResetValues, m_noninherited.misc, "miscReset", true)          \
    G(SizingValues, m_noninherited.sizing, "sizing", true)              \
    G(SurroundValues, m_noninherited.surround, "surround", true)        \
    G(BoxValues, m_noninherited.box, "box", true)

enum class StyleGroupIndex : size_t {
#define LIBWEB_STYLE_GROUP_ENUMERATOR(name, ...) name,
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

    static NonnullRefPtr<ComputedValues const> create(ComputedStyleWorkingSet const&, DOM::Document const&, StyleScope const&, ColorResolutionContext, ComputedValues const* inherit_parent = nullptr);

    // Build only the named groups; every other group keeps `base`'s payload untouched. The caller
    // warrants that every property outside `groups_to_apply` computes to the same value in the
    // given style as it did when `base` was built.
    static constexpr u32 all_style_groups = (1u << to_underlying(StyleGroupIndex::Count)) - 1;
    static NonnullRefPtr<ComputedValues const> create_over_base(ComputedStyleWorkingSet const&, DOM::Document const&, StyleScope const&, ColorResolutionContext, ComputedValues const& base, u32 groups_to_apply);

    // The style group a longhand's computed value lives in, derived from the field descriptors the
    // group payloads build from, plus explicit bindings for the bespoke-built groups. A longhand
    // without a binding has no single known group and must be treated conservatively.
    static Optional<StyleGroupIndex> style_group_of_property(PropertyID);

    RefPtr<StyleValue const> computed_style_value(PropertyID, WithAnimationsApplied = WithAnimationsApplied::Yes) const;
    RefPtr<StyleValue const> computed_style_value_for_inheritance(PropertyID, WithAnimationsApplied = WithAnimationsApplied::Yes) const;

    // The stored Rust style value that IS the property's computed value, for the properties that
    // keep one; null for every other property, and for a stored value that is currently absent.
    // Callers can serialize straight from the returned handle without minting a wrapper.
    RustStyleValueHandle const* stored_style_value_handle(PropertyID) const;

    RefPtr<StyleValue const> color_style_value() const;
    ComputedValues const& base_values() const { return m_borrowed_base_values ? *m_borrowed_base_values : m_base_values ? *m_base_values
                                                                                                                        : *this; }
    bool has_animated_values() const { return m_borrowed_base_values || m_base_values; }
    AnimatedProperties const* animated_properties() const { return m_animated_properties.ptr(); }
    RefPtr<AnimatedProperties const> animated_properties_snapshot() const;

    // Animated values live outside the group payloads, so every group-based fast path or
    // group-based diff must fall back to the slow path when either side carries them.
    static bool either_carries_animated_overlay(ComputedValues const& a, ComputedValues const& b)
    {
        return a.has_animated_values() || b.has_animated_values() || a.animated_properties() || b.animated_properties();
    }

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
    bool differs_in_any_layout_affecting_group_payload_from(ComputedValues const& other) const;

    bool has_transform_style_grouping_property() const;

    // Returns the Rust-owned payload for direct read-only layout access. The
    // pointer is borrowed from this immutable ComputedValues instance.
    void const* style_group_payload(StyleGroupIndex) const;

    // The identity of the half a child inherits. Two styles whose inherited groups are pairwise the
    // same payload answer the same question for a child, whatever their non-inherited halves say.
    static constexpr size_t inherited_style_group_count = 7;
    Array<void const*, inherited_style_group_count> inherited_style_group_identities() const
    {
        return Array<void const*, inherited_style_group_count> {
            m_inherited.table.payload_identity(),
            m_inherited.list.payload_identity(),
            m_inherited.ui.payload_identity(),
            m_inherited.svg.payload_identity(),
            m_inherited.text.payload_identity(),
            m_inherited.box.payload_identity(),
            m_inherited.font.payload_identity(),
        };
    }

    // Calls back with (name, shared_with_parent, is_default) for every style value group,
    // for introspecting how well group sharing is working (see internals.styleGroupSharingInfo()).
    template<typename Callback>
    void for_each_style_group_sharing_state(ComputedValues const* parent, Callback callback) const
    {
#define LIBWEB_VISIT_STYLE_GROUP(name, path, sharing_name, affects_layout) \
    callback(sharing_name##sv, parent ? path.ptr_equals(parent->path) : false, path.is_default());
        LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS(LIBWEB_VISIT_STYLE_GROUP)
#undef LIBWEB_VISIT_STYLE_GROUP
    }

    bool is_property_important(PropertyID property_id) const { return m_property_important.get(property_bitmap_index(property_id)); }
    bool is_property_inherited(PropertyID property_id) const { return m_property_inherited.get(property_bitmap_index(property_id)); }
    ReadonlyBytes property_importance_bitmap() const LIFETIME_BOUND { return m_property_important.bytes(); }
    ReadonlyBytes property_inheritance_bitmap() const LIFETIME_BOUND { return m_property_inherited.bytes(); }

    // True when every inherited longhand took its value by inheritance and no other longhand did:
    // the element's cascade declared nothing that survives into its inherited half, and nothing
    // explicitly inherited a property that does not inherit on its own. Such an element's inherited
    // half is, by construction, exactly what its parent's inherited half was when this style was
    // computed.
    bool property_inheritance_is_standard() const;
    bool depends_on_viewport_metrics() const { return m_depends_on_viewport_metrics; }
    bool font_metrics_depend_on_viewport_metrics() const { return m_font_metrics_depend_on_viewport_metrics; }
    bool in_display_none_subtree() const { return m_in_display_none_subtree; }
    bool has_pseudo_element_style(PseudoElement pseudo_element) const { return m_pseudo_element_styles & (1ull << to_underlying(pseudo_element)); }
    u64 pseudo_element_style_mask() const { return m_pseudo_element_styles; }
    HashMap<PropertyID, NonnullRefPtr<StyleValue const>> const& inheritance_dependent_specified_values() const { return m_inheritance_dependent_specified_values; }
    bool inheritance_dependent_specified_values_equal(ComputedValues const& other) const;
    ReadonlySpan<StyleEngineFFI::FfiInheritanceDependentValue const> borrowed_inheritance_dependent_values() const { return m_borrowed_inheritance_dependent_values; }
    HashMap<PropertyID, NonnullRefPtr<StyleValue const>> inheritance_dependent_specified_values_snapshot() const;
    RefPtr<StyleValue const> raw_cascaded_font_size() const;

    // The drive's frozen computed longhand table (a Rust ComputedLonghandTable), or null when
    // this style holds only a borrowed span or no table at all.
    void const* computed_longhand_table() const { return m_computed_longhand_table; }
    // One stored data pointer per longhand (null slots where the drive stored no value); empty
    // when this style carries no table. The pointers stay valid while this ComputedValues is
    // live (or, for a borrowed record view, while the style engine retains the record's table).
    ReadonlySpan<void const*> computed_longhand_values() const { return m_longhand_values; }

    ~ComputedValues();

private:
    static NonnullRefPtr<ComputedValues const> create_internal(ComputedStyleWorkingSet const&, DOM::Document const&, StyleScope const&, ColorResolutionContext, ComputedValues const* inherit_parent, ComputedValues const* base, u32 groups_to_apply);

public:
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
    ReadonlySpan<Utf16FlyString> anchor_names() const { return m_noninherited.anchor->anchor_names_span(); }
    AnchorScopeData anchor_scope() const { return m_noninherited.anchor->anchor_scope_value(); }
    Vector<ComputedAnimationName> animation_names() const { return m_noninherited.animation->animation_names_value(); }
    Vector<AnimationComposition> animation_compositions() const { return m_noninherited.animation->animation_compositions_value(); }
    Vector<Time> animation_delays() const { return m_noninherited.animation->animation_delays_value(); }
    Vector<AnimationDirection> animation_directions() const { return m_noninherited.animation->animation_directions_value(); }
    Vector<Optional<Time>> animation_durations() const { return m_noninherited.animation->animation_durations_value(); }
    Vector<AnimationFillMode> animation_fill_modes() const { return m_noninherited.animation->animation_fill_modes_value(); }
    Vector<double> animation_iteration_counts() const { return m_noninherited.animation->animation_iteration_counts_value(); }
    Vector<AnimationPlayState> animation_play_states() const { return m_noninherited.animation->animation_play_states_value(); }
    Vector<AnimationTimelineData> animation_timelines() const { return m_noninherited.animation->animation_timelines_value(); }
    Vector<EasingFunction> animation_timing_functions() const { return m_noninherited.animation->animation_timing_functions_value(); }
    StyleValueVector animation_timing_function_style_values() const { return m_noninherited.animation->animation_timing_function_style_values_value(); }
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
    Color caret_color() const { return m_inherited.ui->caret_color_value(); }
    Clear clear() const { return static_cast<Clear>(m_noninherited.box->clear); }
    Clip clip() const { return m_noninherited.effects->clip_value(); }
    ColorInterpolation color_interpolation() const { return m_inherited.svg->color_interpolation_value(); }
    ColorInterpolation color_interpolation_filters() const { return m_inherited.svg->color_interpolation_filters_value(); }
    PreferredColorScheme color_scheme() const { return m_inherited.ui->color_scheme_value(); }
    ReadonlySpan<Utf16FlyString> color_schemes() const { return m_inherited.ui->color_schemes_span(); }
    bool color_scheme_only() const { return m_inherited.ui->color_scheme_only; }
    ContentVisibility content_visibility() const { return static_cast<ContentVisibility>(m_inherited.box->content_visibility); }
    ReadonlySpan<ComputedValuesFFI::ComputedCursor> cursor() const { return m_inherited.ui->cursor_span(); }
    ComputedContentData computed_content() const { return m_noninherited.content_data->computed_content_value(); }
    ContentDataAndQuoteNestingLevel resolved_content(DOM::AbstractElement&, u32 initial_quote_nesting_level) const;
    Vector<CounterData, 0> counter_increment() const { return m_noninherited.content_data->counter_increment_value(); }
    Vector<CounterData, 0> counter_reset() const { return m_noninherited.content_data->counter_reset_value(); }
    Vector<CounterData, 0> counter_set() const { return m_noninherited.content_data->counter_set_value(); }
    PointerEvents pointer_events() const { return m_inherited.ui->pointer_events_value(); }
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
        return m_inherited.text->tab_size_length_value();
    }
    TextAlign text_align() const { return m_inherited.text->text_align_value(); }
    TextJustify text_justify() const { return m_inherited.text->text_justify_value(); }
    TextIndentData text_indent() const { return m_inherited.text->text_indent_value(); }
    TextWrapMode text_wrap_mode() const { return m_inherited.text->text_wrap_mode_value(); }
    TextWrapStyle text_wrap_style() const { return m_inherited.text->text_wrap_style_value(); }
    CSSPixels text_underline_offset() const { return m_inherited.text->text_underline_offset_value(); }
    TextUnderlinePosition text_underline_position() const { return m_inherited.text->text_underline_position_value(); }
    ReadonlySpan<TextDecorationLine> text_decoration_line() const { return m_noninherited.text_reset->decoration_lines(); }
    TextDecorationThickness text_decoration_thickness() const { return m_noninherited.text_reset->decoration_thickness(); }
    TextDecorationSkipInk text_decoration_skip_ink() const { return m_inherited.text->text_decoration_skip_ink_value(); }
    TextDecorationStyle text_decoration_style() const { return static_cast<TextDecorationStyle>(m_noninherited.text_reset->text_decoration_style); }
    Color text_decoration_color() const { return Color::from_bgra(m_noninherited.text_reset->text_decoration_color); }
    TextTransform text_transform() const { return m_inherited.text->text_transform_value(); }
    TextOverflow text_overflow() const { return static_cast<TextOverflow>(m_noninherited.box->text_overflow); }
    ReadonlySpan<ShadowData> text_shadow() const { return m_inherited.text->text_shadow_span(); }
    Positioning position() const { return static_cast<Positioning>(m_noninherited.box->position); }
    PositionAnchor position_anchor_value() const { return m_noninherited.anchor->position_anchor_value(); }
    Optional<Utf16FlyString> position_anchor() const { return m_noninherited.anchor->position_anchor_value().name; }
    PositionAreaData position_area() const { return m_noninherited.anchor->position_area_value(); }
    Vector<PositionTryFallbackData> position_try_fallbacks() const { return m_noninherited.anchor->position_try_fallbacks_value(); }
    Optional<TryOrder> position_try_order() const { return m_noninherited.anchor->position_try_order_value(); }
    PositionVisibilityData position_visibility() const { return m_noninherited.anchor->position_visibility_value(); }
    Vector<Optional<Utf16FlyString>> scroll_timeline_names() const { return m_noninherited.animation->scroll_timeline_names_value(); }
    Vector<Axis> scroll_timeline_axes() const { return m_noninherited.animation->scroll_timeline_axes_value(); }
    TimelineScopeData timeline_scope() const { return m_noninherited.animation->timeline_scope_value(); }
    Vector<Optional<Utf16FlyString>> view_timeline_names() const { return m_noninherited.animation->view_timeline_names_value(); }
    Vector<Axis> view_timeline_axes() const { return m_noninherited.animation->view_timeline_axes_value(); }
    Vector<ViewTimelineInsetData> view_timeline_insets() const { return m_noninherited.animation->view_timeline_insets_value(); }
    Vector<Optional<Utf16FlyString>> transition_properties() const { return m_noninherited.animation->transition_properties_value(); }
    Vector<Time> transition_durations() const { return m_noninherited.animation->transition_durations_value(); }
    Vector<EasingFunction> transition_timing_functions() const { return m_noninherited.animation->transition_timing_functions_value(); }
    StyleValueVector transition_timing_function_style_values() const { return m_noninherited.animation->transition_timing_function_style_values_value(); }
    Vector<Time> transition_delays() const { return m_noninherited.animation->transition_delays_value(); }
    Vector<TransitionBehavior> transition_behaviors() const { return m_noninherited.animation->transition_behaviors_value(); }
    bool transition_delay_and_duration_are_single_zero() const { return m_noninherited.animation->transition_delay_and_duration_are_single_zero_value(); }
    WhiteSpaceCollapse white_space_collapse() const { return m_inherited.text->white_space_collapse_value(); }
    WhiteSpaceTrimData white_space_trim() const { return m_noninherited.text_reset->white_space_trim(); }
    WordBreak word_break() const { return m_inherited.text->word_break_value(); }
    OverflowWrap overflow_wrap() const { return m_inherited.text->overflow_wrap_value(); }
    u64 orphans() const { return m_inherited.text->orphans; }
    u64 widows() const { return m_inherited.text->widows; }
    FontVariantEmoji font_variant_emoji() const { return static_cast<FontVariantEmoji>(m_inherited.font->font_variant_emoji); }
    CSSPixels word_spacing() const { return m_inherited.text->word_spacing_value(); }
    CSSPixels letter_spacing() const { return m_inherited.text->letter_spacing_value(); }
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
    Optional<Color> accent_color() const
    {
        return m_inherited.ui->accent_color_value();
    }
    AlignContent align_content() const { return static_cast<AlignContent>(m_noninherited.alignment->align_content); }
    AlignItems align_items() const { return static_cast<AlignItems>(m_noninherited.alignment->align_items); }
    AlignSelf align_self() const { return static_cast<AlignSelf>(m_noninherited.alignment->align_self); }
    Appearance appearance() const { return static_cast<Appearance>(m_noninherited.misc->appearance); }
    Appearance computed_appearance() const { return static_cast<Appearance>(m_noninherited.misc->computed_appearance); }
    float opacity() const { return m_noninherited.effects->opacity; }
    Visibility visibility() const { return static_cast<Visibility>(m_inherited.box->visibility); }
    ImageRendering image_rendering() const { return static_cast<ImageRendering>(m_inherited.box->image_rendering); }
    JustifyContent justify_content() const { return static_cast<JustifyContent>(m_noninherited.alignment->justify_content); }
    JustifySelf justify_self() const { return static_cast<JustifySelf>(m_noninherited.alignment->justify_self); }
    JustifyItems justify_items() const { return static_cast<JustifyItems>(m_noninherited.alignment->justify_items); }
    ComputedFilterView backdrop_filter() const { return m_noninherited.effects->backdrop_filter_value(); }
    ComputedFilterView filter() const { return m_noninherited.effects->filter_value(); }
    ReadonlySpan<ShadowData> box_shadow() const { return m_noninherited.effects->box_shadow_span(); }
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
    GridAutoFlow grid_auto_flow() const
    {
        return { .row = m_noninherited.box->grid_auto_flow_row, .dense = m_noninherited.box->grid_auto_flow_dense };
    }
    ColumnCount column_count() const
    {
        if (!m_noninherited.box->column_count_has_value)
            return ColumnCount::make_auto();
        return ColumnCount::make_integer(m_noninherited.box->column_count);
    }
    Variant<LengthPercentage, NormalGap> column_gap() const { return gap(m_noninherited.alignment->column_gap); }
    ColumnSpan column_span() const { return static_cast<ColumnSpan>(m_noninherited.misc->column_span); }
    Size const& column_width() const { return Size::view(m_noninherited.box->column_width); }
    Size const& column_height() const { return Size::view(m_noninherited.misc->column_height); }
    Variant<LengthPercentage, NormalGap> row_gap() const { return gap(m_noninherited.alignment->row_gap); }
    BorderCollapse border_collapse() const { return static_cast<BorderCollapse>(m_inherited.table->border_collapse); }
    EmptyCells empty_cells() const { return static_cast<EmptyCells>(m_inherited.table->empty_cells); }
    ObjectFit object_fit() const { return static_cast<ObjectFit>(m_noninherited.misc->object_fit); }
    Position object_position() const { return m_noninherited.misc->object_position_value(); }
    Direction direction() const { return static_cast<Direction>(m_inherited.box->direction); }
    Optional<BaselineMetric> dominant_baseline() const { return m_inherited.svg->dominant_baseline_value(); }
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

    UserSelect user_select() const { return static_cast<UserSelect>(m_noninherited.misc->user_select); }
    Isolation isolation() const { return m_noninherited.effects->isolation_value(); }
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
    MixBlendMode mix_blend_mode() const { return m_noninherited.effects->mix_blend_mode_value(); }
    Optional<Utf16FlyString> view_transition_name() const { return m_noninherited.misc->view_transition_name_value(); }
    TouchActionData touch_action() const { return m_noninherited.misc->touch_action_value(); }
    ShapeRendering shape_rendering() const { return m_inherited.svg->shape_rendering_value(); }

    LengthBox inset() const { return length_box(m_noninherited.surround->inset); }
    bool has_anchor_inset(PropertyID property_id) const
    {
        auto const* handle = anchor_inset_handle(property_id);
        return handle && handle->pointer != nullptr;
    }
    bool inset_properties_contain_anchor_functions() const;
    RefPtr<StyleValue const> anchor_inset(PropertyID property_id) const
    {
        auto const* handle = anchor_inset_handle(property_id);
        if (!handle)
            return {};
        static_assert(sizeof(RustStyleValueHandle) == sizeof(*handle));
        return style_value_from_handle(property_id, reinterpret_cast<RustStyleValueHandle const&>(*handle));
    }
    LengthBox margin() const { return length_box(m_noninherited.surround->margin); }
    LengthBox padding() const { return length_box(m_noninherited.surround->padding); }
    LengthBox scroll_margin() const { return length_box(m_noninherited.misc->scroll_margin); }
    LengthBox scroll_padding() const { return length_box(m_noninherited.misc->scroll_padding); }
    OverflowClipMarginData overflow_clip_margin() const { return m_noninherited.misc->overflow_clip_margin_value(); }

    BorderData const& border_left() const { return m_noninherited.border->border_left_value(); }
    BorderData const& border_top() const { return m_noninherited.border->border_top_value(); }
    BorderData const& border_right() const { return m_noninherited.border->border_right_value(); }
    BorderData const& border_bottom() const { return m_noninherited.border->border_bottom_value(); }
    CSSPixels border_left_computed_width() const { return m_noninherited.border->border_left_computed_width_value(); }
    CSSPixels border_top_computed_width() const { return m_noninherited.border->border_top_computed_width_value(); }
    CSSPixels border_right_computed_width() const { return m_noninherited.border->border_right_computed_width_value(); }
    CSSPixels border_bottom_computed_width() const { return m_noninherited.border->border_bottom_computed_width_value(); }

    bool has_noninitial_border_radii() const { return m_noninherited.border->has_noninitial_border_radii_value(); }
    BorderRadiusData border_bottom_left_radius() const { return m_noninherited.border->border_bottom_left_radius_value(); }
    BorderRadiusData border_bottom_right_radius() const { return m_noninherited.border->border_bottom_right_radius_value(); }
    BorderRadiusData border_top_left_radius() const { return m_noninherited.border->border_top_left_radius_value(); }
    BorderRadiusData border_top_right_radius() const { return m_noninherited.border->border_top_right_radius_value(); }
    double corner_bottom_left_shape() const { return m_noninherited.border->corner_bottom_left_shape; }
    double corner_bottom_right_shape() const { return m_noninherited.border->corner_bottom_right_shape; }
    double corner_top_left_shape() const { return m_noninherited.border->corner_top_left_shape; }
    double corner_top_right_shape() const { return m_noninherited.border->corner_top_right_shape; }

    Overflow overflow_x() const { return static_cast<Overflow>(m_noninherited.box->overflow_x); }
    Overflow overflow_y() const { return static_cast<Overflow>(m_noninherited.box->overflow_y); }

    Color color() const { return m_inherited.text->color_value(); }
    Color background_color() const { return m_noninherited.background->background_color_value(); }
    RefPtr<StyleValue const> background_color_style_value() const;
    BackgroundBox background_color_clip() const { return m_noninherited.background->background_color_clip_value(); }
    Vector<BackgroundLayerData> background_layers() const { return m_noninherited.background->background_layers_value(); }
    Vector<BackgroundLayerData> mask_layers() const { return m_noninherited.mask_data->mask_layers_value(); }
    Vector<Position> mask_positions() const { return m_noninherited.mask_data->mask_positions_value(); }
    BorderImageData border_image() const { return m_noninherited.border->border_image_value(); }

    Color webkit_text_fill_color() const { return m_inherited.text->webkit_text_fill_color_value(); }
    bool webkit_text_fill_color_is_current_color() const { return m_inherited.text->webkit_text_fill_color_is_current_color; }

    ListStyleType list_style_type(StyleScope const& style_scope) const { return m_inherited.list->list_style_type_value(style_scope); }
    bool list_style_type_depends_on_counter_style_environment() const { return m_inherited.list->list_style_type_depends_on_counter_style_environment(); }
    ListStylePosition list_style_position() const { return static_cast<ListStylePosition>(m_inherited.list->list_style_position); }

    Optional<SVGPaint> fill() const { return m_inherited.svg->fill_value(); }
    FillRule fill_rule() const { return m_inherited.svg->fill_rule_value(); }
    Optional<SVGPaint> stroke() const { return m_inherited.svg->stroke_value(); }
    float fill_opacity() const { return m_inherited.svg->fill_opacity; }
    ReadonlySpan<ComputedValuesFFI::ComputedSvgDash> stroke_dasharray() const { return m_inherited.svg->stroke_dasharray_span(); }
    LengthPercentage const& stroke_dashoffset() const { return m_inherited.svg->stroke_dashoffset_value(); }
    StrokeLinecap stroke_linecap() const { return m_inherited.svg->stroke_linecap_value(); }
    StrokeLinejoin stroke_linejoin() const { return m_inherited.svg->stroke_linejoin_value(); }
    VectorEffect vector_effect() const { return static_cast<VectorEffect>(m_noninherited.svg_reset->vector_effect); }
    double stroke_miterlimit() const { return m_inherited.svg->stroke_miterlimit; }
    float stroke_opacity() const { return m_inherited.svg->stroke_opacity; }
    LengthPercentage const& stroke_width() const { return m_inherited.svg->stroke_width_value(); }
    Color stop_color() const { return Gfx::Color::from_bgra(m_noninherited.svg_reset->stop_color); }
    float stop_opacity() const { return m_noninherited.svg_reset->stop_opacity; }
    TextAnchor text_anchor() const { return m_inherited.svg->text_anchor_value(); }
    RefPtr<AbstractImageStyleValue const> mask_image() const { return m_noninherited.mask_data->mask_image_value(); }
    Optional<MaskReference> mask() const { return m_noninherited.mask_data->mask_value(); }
    MaskType mask_type() const { return m_noninherited.mask_data->mask_type_value(); }
    Optional<ClipPathReference> clip_path() const { return m_noninherited.mask_data->clip_path_value(); }
    ClipRule clip_rule() const { return m_inherited.svg->clip_rule_value(); }
    Color flood_color() const { return Gfx::Color::from_bgra(m_noninherited.svg_reset->flood_color); }
    float flood_opacity() const { return m_noninherited.svg_reset->flood_opacity; }
    PaintOrderList paint_order() const { return m_inherited.svg->paint_order_value(); }
    u8 paint_order_serialization_length() const { return m_inherited.svg->paint_order_serialization_length; }
    bool paint_order_is_normal() const { return m_inherited.svg->paint_order_is_normal; }

    LengthPercentage const& cx() const { return LengthPercentage::view(m_noninherited.svg_reset->cx); }
    LengthPercentage const& cy() const { return LengthPercentage::view(m_noninherited.svg_reset->cy); }
    NonnullRefPtr<StyleValue const> d() const
    {
        auto const* handle = &m_noninherited.svg_reset->d;
        static_assert(sizeof(RustStyleValueHandle) == sizeof(*handle));
        return style_value_from_handle(PropertyID::D, reinterpret_cast<RustStyleValueHandle const&>(*handle)).release_nonnull();
    }
    LengthPercentage const& r() const { return LengthPercentage::view(m_noninherited.svg_reset->r); }
    LengthPercentageOrAuto rx() const { return m_noninherited.svg_reset->rx.is_auto ? LengthPercentageOrAuto::make_auto() : LengthPercentage::view(m_noninherited.svg_reset->rx.value); }
    LengthPercentageOrAuto ry() const { return m_noninherited.svg_reset->ry.is_auto ? LengthPercentageOrAuto::make_auto() : LengthPercentage::view(m_noninherited.svg_reset->ry.value); }
    LengthPercentage const& x() const { return LengthPercentage::view(m_noninherited.svg_reset->x); }
    LengthPercentage const& y() const { return LengthPercentage::view(m_noninherited.svg_reset->y); }

    bool has_transformations() const { return m_noninherited.transform->has_transformations(); }
    template<typename Callback>
    void for_each_transformation(Callback callback) const
    {
        m_noninherited.transform->for_each_transformation(callback);
    }
    bool has_resolved_transforms() const { return m_noninherited.transform->has_resolved_transforms(); }
    template<typename Callback>
    void for_each_resolved_transform(Callback callback) const
    {
        m_noninherited.transform->for_each_resolved_transform(callback);
    }
    TransformBox transform_box() const { return m_noninherited.transform->transform_box_value(); }
    TransformOrigin transform_origin() const { return m_noninherited.transform->transform_origin_value(); }
    TransformStyle transform_style() const { return m_noninherited.transform->transform_style_value(); }
    BackfaceVisibility backface_visibility() const { return m_noninherited.transform->backface_visibility_value(); }
    RefPtr<TransformationStyleValue const> rotate() const { return m_noninherited.transform->rotate_value(); }
    RefPtr<TransformationStyleValue const> translate() const { return m_noninherited.transform->translate_value(); }
    RefPtr<TransformationStyleValue const> scale() const { return m_noninherited.transform->scale_value(); }
    bool has_rotate() const { return rotate() != nullptr; }
    bool has_translate() const { return translate() != nullptr; }
    bool has_scale() const { return scale() != nullptr; }
    Optional<CSSPixels> perspective() const { return m_noninherited.transform->perspective_value(); }
    Position perspective_origin() const { return m_noninherited.transform->perspective_origin_value(); }

    Gfx::FontCascadeList const& font_list() const { return m_inherited.font->font_list_value(); }
    CSSPixels font_size() const { return CSSPixels::from_raw(m_inherited.font->font_size); }
    double font_weight() const { return m_inherited.font->font_weight; }
    Percentage font_width() const { return Percentage { m_inherited.font->font_width }; }
    CSSPixels line_height() const { return CSSPixels::from_raw(m_inherited.font->line_height_used); }

    Color outline_color() const { return Color::from_bgra(m_noninherited.misc->outline_color); }
    CSSPixels outline_offset() const { return CSSPixels::from_raw(m_noninherited.misc->outline_offset); }
    RefPtr<StyleValue const> outline_offset_style_value() const { return m_noninherited.misc->outline_offset_style_value_value(); }
    OutlineStyle outline_style() const { return static_cast<OutlineStyle>(m_noninherited.misc->outline_style); }
    CSSPixels outline_width() const { return CSSPixels::from_raw(m_noninherited.misc->outline_width); }

    TableLayout table_layout() const { return static_cast<TableLayout>(m_noninherited.box->table_layout); }

    QuotesData quotes() const { return m_inherited.list->quotes_value(); }

    MathShift math_shift() const { return static_cast<MathShift>(m_inherited.font->math_shift); }
    MathStyle math_style() const { return static_cast<MathStyle>(m_inherited.font->math_style); }
    int math_depth() const { return m_inherited.font->math_depth; }

    ScrollBehavior scroll_behavior() const { return static_cast<ScrollBehavior>(m_noninherited.misc->scroll_behavior); }
    ScrollbarColorData scrollbar_color() const { return m_inherited.ui->scrollbar_color_value(); }
    ScrollbarGutter scrollbar_gutter() const { return static_cast<ScrollbarGutter>(m_noninherited.misc->scrollbar_gutter); }
    ScrollbarWidth scrollbar_width() const { return static_cast<ScrollbarWidth>(m_noninherited.misc->scrollbar_width); }
    Resize resize() const { return static_cast<Resize>(m_noninherited.box->resize); }
    double shape_image_threshold() const { return m_noninherited.misc->shape_image_threshold; }
    LengthPercentage shape_margin() const { return LengthPercentage::view(m_noninherited.misc->shape_margin); }
    ShapeOutsideData shape_outside() const { return m_noninherited.misc->shape_outside_value(); }
    WillChange will_change() const { return m_noninherited.misc->will_change_value(); }

private:
    friend class ComputedStyleRecordView;
    enum class BorrowedStyleRecord { Yes };
    ComputedValues();
    explicit ComputedValues(BorrowedStyleRecord);
    void borrow_style_record_payloads(ReadonlySpan<void const*>);

    RefPtr<StyleValue const> style_value_from_handle(PropertyID, RustStyleValueHandle const&) const;

    ComputedValuesFFI::ComputedStyleValueHandle const* anchor_inset_handle(PropertyID property_id) const
    {
        switch (property_id) {
        case PropertyID::Top:
            return &m_noninherited.surround->top_anchor_inset;
        case PropertyID::Right:
            return &m_noninherited.surround->right_anchor_inset;
        case PropertyID::Bottom:
            return &m_noninherited.surround->bottom_anchor_inset;
        case PropertyID::Left:
            return &m_noninherited.surround->left_anchor_inset;
        default:
            return nullptr;
        }
    }

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

    struct InheritedListValues : ComputedValuesFFI::InheritedListValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::InheritedListValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::InheritedList;

        ListStyleType list_style_type_value(StyleScope const&) const;
        bool list_style_type_depends_on_counter_style_environment() const;
        RefPtr<AbstractImageStyleValue const> list_style_image_value() const;
        QuotesData quotes_value() const;

        bool operator==(InheritedListValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
    };

    struct InheritedUIValues : ComputedValuesFFI::InheritedUIValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::InheritedUIValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::InheritedUI;

        Color caret_color_value() const { return Color::from_bgra(caret_color.used_color); }
        Optional<Color> accent_color_value() const
        {
            if (accent_color.is_auto)
                return {};
            return Color::from_bgra(accent_color.used_color);
        }
        ReadonlySpan<ComputedValuesFFI::ComputedCursor> cursor_span() const { return { cursor.pointer, cursor.length }; }
        static RefPtr<CursorStyleValue const> cursor_style_value(ComputedValuesFFI::ComputedCursor const& value)
        {
            if (!value.is_cursor_value)
                return nullptr;
            auto style_value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                static_cast<StyleValueFFI::StyleValueData const*>(value.cursor.pointer)));
            return style_value->as_cursor();
        }
        PointerEvents pointer_events_value() const { return static_cast<PointerEvents>(pointer_events); }
        ScrollbarColorData scrollbar_color_value() const
        {
            return {
                .thumb_color = Color::from_bgra(scrollbar_color.thumb_color),
                .track_color = Color::from_bgra(scrollbar_color.track_color),
                .is_auto = scrollbar_color.is_auto,
            };
        }
        PreferredColorScheme color_scheme_value() const { return static_cast<PreferredColorScheme>(color_scheme); }
        ReadonlySpan<Utf16FlyString> color_schemes_span() const
        {
            static_assert(sizeof(Utf16FlyString) == sizeof(size_t));
            static_assert(alignof(Utf16FlyString) == alignof(size_t));
            return { reinterpret_cast<Utf16FlyString const*>(color_schemes.raw_pointer), color_schemes.length };
        }

        bool operator==(InheritedUIValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
    };

    struct InheritedSVGValues : ComputedValuesFFI::InheritedSVGValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::InheritedSVGValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::InheritedSVG;

        static Optional<SVGPaint> paint_value(ComputedValuesFFI::ComputedSvgPaint const& paint)
        {
            switch (paint.kind) {
            case 0:
                return {};
            case 1:
                return SVGPaint { Color::from_bgra(paint.color), paint.color_is_currentcolor };
            case 2: {
                auto style_value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                    static_cast<StyleValueFFI::StyleValueData const*>(paint.url.pointer)));
                Optional<Color> fallback_color;
                if (paint.has_color)
                    fallback_color = Color::from_bgra(paint.color);
                return SVGPaint { style_value->as_url().url(), fallback_color, paint.color_is_currentcolor };
            }
            default:
                VERIFY_NOT_REACHED();
            }
        }

        Optional<SVGPaint> fill_value() const { return paint_value(fill); }
        Optional<SVGPaint> stroke_value() const { return paint_value(stroke); }
        FillRule fill_rule_value() const { return static_cast<FillRule>(fill_rule); }
        ClipRule clip_rule_value() const { return static_cast<ClipRule>(clip_rule); }
        StrokeLinecap stroke_linecap_value() const { return static_cast<StrokeLinecap>(stroke_linecap); }
        StrokeLinejoin stroke_linejoin_value() const { return static_cast<StrokeLinejoin>(stroke_linejoin); }
        ColorInterpolation color_interpolation_value() const { return static_cast<ColorInterpolation>(color_interpolation); }
        ColorInterpolation color_interpolation_filters_value() const { return static_cast<ColorInterpolation>(color_interpolation_filters); }
        TextAnchor text_anchor_value() const { return static_cast<TextAnchor>(text_anchor); }
        ShapeRendering shape_rendering_value() const { return static_cast<ShapeRendering>(shape_rendering); }
        ReadonlySpan<ComputedValuesFFI::ComputedSvgDash> stroke_dasharray_span() const { return { stroke_dasharray.pointer, stroke_dasharray.length }; }
        LengthPercentage const& stroke_dashoffset_value() const { return LengthPercentage::view(stroke_dashoffset); }
        LengthPercentage const& stroke_width_value() const { return LengthPercentage::view(stroke_width); }
        PaintOrderList paint_order_value() const
        {
            return {
                static_cast<PaintOrder>(paint_order[0]),
                static_cast<PaintOrder>(paint_order[1]),
                static_cast<PaintOrder>(paint_order[2]),
            };
        }
        Optional<BaselineMetric> dominant_baseline_value() const
        {
            if (!has_dominant_baseline)
                return {};
            return static_cast<BaselineMetric>(dominant_baseline);
        }

        bool operator==(InheritedSVGValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
    };

    struct InheritedTextValues : ComputedValuesFFI::InheritedTextValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::InheritedTextValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::InheritedText;

        TextAlign text_align_value() const { return static_cast<TextAlign>(text_align); }
        TextJustify text_justify_value() const { return static_cast<TextJustify>(text_justify); }
        WhiteSpaceCollapse white_space_collapse_value() const { return static_cast<WhiteSpaceCollapse>(white_space_collapse); }
        TextWrapMode text_wrap_mode_value() const { return static_cast<TextWrapMode>(text_wrap_mode); }
        WordBreak word_break_value() const { return static_cast<WordBreak>(word_break); }
        CSSPixels letter_spacing_value() const { return CSSPixels::from_raw(letter_spacing); }
        CSSPixels word_spacing_value() const { return CSSPixels::from_raw(word_spacing); }
        CSSPixels tab_size_length_value() const { return CSSPixels::from_raw(tab_size_length); }
        TextIndentData text_indent_value() const
        {
            return {
                .length_percentage = LengthPercentage::view(text_indent.length_percentage),
                .each_line = text_indent.each_line,
                .hanging = text_indent.hanging,
            };
        }
        Color color_value() const { return Color::from_bgra(color); }
        Color webkit_text_fill_color_value() const { return Color::from_bgra(webkit_text_fill_color); }
        ReadonlySpan<ShadowData> text_shadow_span() const
        {
            static_assert(sizeof(ShadowData) == sizeof(ComputedValuesFFI::ComputedShadow));
            return { reinterpret_cast<ShadowData const*>(text_shadow.pointer), text_shadow.length };
        }
        TextTransform text_transform_value() const { return static_cast<TextTransform>(text_transform); }
        TextWrapStyle text_wrap_style_value() const { return static_cast<TextWrapStyle>(text_wrap_style); }
        TextDecorationSkipInk text_decoration_skip_ink_value() const { return static_cast<TextDecorationSkipInk>(text_decoration_skip_ink); }
        TextUnderlinePosition text_underline_position_value() const
        {
            return {
                .horizontal = static_cast<TextUnderlinePositionHorizontal>(text_underline_position.horizontal),
                .vertical = static_cast<TextUnderlinePositionVertical>(text_underline_position.vertical),
            };
        }
        CSSPixels text_underline_offset_value() const { return CSSPixels::from_raw(text_underline_offset.used_value); }
        OverflowWrap overflow_wrap_value() const { return static_cast<OverflowWrap>(overflow_wrap); }

        bool operator==(InheritedTextValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
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

    // Rust owns the canonical font values and the derived layout-facing facts.
    // The platform font pointers borrow cascades pinned by FontComputer.
    struct FontValues : ComputedValuesFFI::FontValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::FontValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::Font;

        WEB_API Gfx::FontCascadeList const& font_list_value() const;
        RefPtr<StyleValue const> font_family_style_value() const;

        bool operator==(FontValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
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
    struct AnimationValues : ComputedValuesFFI::AnimationValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::AnimationValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::Animation;

        Vector<ComputedAnimationName> animation_names_value() const;
        Vector<AnimationComposition> animation_compositions_value() const;
        Vector<Time> animation_delays_value() const;
        Vector<AnimationDirection> animation_directions_value() const;
        Vector<Optional<Time>> animation_durations_value() const;
        Vector<AnimationFillMode> animation_fill_modes_value() const;
        Vector<double> animation_iteration_counts_value() const;
        Vector<AnimationPlayState> animation_play_states_value() const;
        Vector<AnimationTimelineData> animation_timelines_value() const;
        Vector<EasingFunction> animation_timing_functions_value() const;
        StyleValueVector animation_timing_function_style_values_value() const;
        Vector<Optional<Utf16FlyString>> scroll_timeline_names_value() const;
        Vector<Axis> scroll_timeline_axes_value() const;
        TimelineScopeData timeline_scope_value() const;
        Vector<Optional<Utf16FlyString>> view_timeline_names_value() const;
        Vector<Axis> view_timeline_axes_value() const;
        Vector<ViewTimelineInsetData> view_timeline_insets_value() const;
        Vector<Optional<Utf16FlyString>> transition_properties_value() const;
        Vector<Time> transition_durations_value() const;
        Vector<EasingFunction> transition_timing_functions_value() const;
        StyleValueVector transition_timing_function_style_values_value() const;
        Vector<Time> transition_delays_value() const;
        Vector<TransitionBehavior> transition_behaviors_value() const;
        bool transition_delay_and_duration_are_single_zero_value() const { return transition_delay_and_duration_are_single_zero; }

        bool operator==(AnimationValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
    };

    // The layout and lifecycle of this group are defined in Rust (computed_values.rs).
    struct SVGResetValues : ComputedValuesFFI::SVGResetValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::SVGResetValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::SVGReset;

        bool operator==(SVGResetValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
    };

    struct GridValues : ComputedValuesFFI::GridValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::GridValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::Grid;

        bool operator==(GridValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
    };

    struct AnchorValues : ComputedValuesFFI::AnchorValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::AnchorValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::Anchor;

        ReadonlySpan<Utf16FlyString> anchor_names_span() const { return fly_strings(anchor_names); }
        AnchorScopeData anchor_scope_value() const
        {
            return { .all = anchor_scope_all, .names = materialize_fly_strings(anchor_scope_names) };
        }
        PositionAnchor position_anchor_value() const
        {
            PositionAnchor value {
                .type = static_cast<PositionAnchor::Type>(position_anchor_type),
                .name = {},
            };
            if (value.type == PositionAnchor::Type::Name)
                value.name = Utf16FlyString::from_raw(position_anchor_name.raw);
            return value;
        }
        PositionAreaData position_area_value() const { return materialize_position_area(position_area); }
        Vector<PositionTryFallbackData> position_try_fallbacks_value() const
        {
            Vector<PositionTryFallbackData> values;
            values.ensure_capacity(position_try_fallbacks.length);
            for (size_t index = 0; index < position_try_fallbacks.length; ++index) {
                auto const& source = position_try_fallbacks.pointer[index];
                PositionTryFallbackData value;
                if (source.name.raw)
                    value.name = Utf16FlyString::from_raw(source.name.raw);
                value.tactics.ensure_capacity(source.tactic_count);
                for (size_t tactic = 0; tactic < source.tactic_count; ++tactic)
                    value.tactics.unchecked_append(static_cast<TryTactic>(source.tactics[tactic]));
                if (source.has_position_area)
                    value.position_area = materialize_position_area(source.position_area);
                values.unchecked_append(move(value));
            }
            return values;
        }
        Optional<TryOrder> position_try_order_value() const
        {
            if (!has_position_try_order)
                return {};
            return static_cast<TryOrder>(position_try_order);
        }
        PositionVisibilityData position_visibility_value() const
        {
            return {
                .always = position_visibility_always,
                .anchors_valid = position_visibility_anchors_valid,
                .anchors_visible = position_visibility_anchors_visible,
                .no_overflow = position_visibility_no_overflow,
            };
        }

        bool operator==(AnchorValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }

    private:
        static ReadonlySpan<Utf16FlyString> fly_strings(ComputedValuesFFI::RetainedUtf16FlyStringList const& values)
        {
            static_assert(sizeof(Utf16FlyString) == sizeof(size_t));
            static_assert(alignof(Utf16FlyString) == alignof(size_t));
            return { reinterpret_cast<Utf16FlyString const*>(values.raw_pointer), values.length };
        }
        static Vector<Utf16FlyString> materialize_fly_strings(ComputedValuesFFI::RetainedUtf16FlyStringList const& values)
        {
            Vector<Utf16FlyString> result;
            result.ensure_capacity(values.length);
            for (auto const& value : fly_strings(values))
                result.unchecked_append(value);
            return result;
        }
        static PositionAreaData materialize_position_area(ComputedValuesFFI::RetainedPositionAreaList const& values)
        {
            PositionAreaData result;
            result.keywords.ensure_capacity(values.length);
            for (size_t index = 0; index < values.length; ++index)
                result.keywords.unchecked_append(static_cast<PositionArea>(values.pointer[index]));
            return result;
        }
    };

    struct EffectsValues : ComputedValuesFFI::EffectsValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::EffectsValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::Effects;

        ComputedFilterView filter_value() const { return ComputedFilterView { filter }; }
        ComputedFilterView backdrop_filter_value() const { return ComputedFilterView { backdrop_filter }; }
        MixBlendMode mix_blend_mode_value() const { return static_cast<MixBlendMode>(mix_blend_mode); }
        Isolation isolation_value() const { return static_cast<Isolation>(isolation); }
        ReadonlySpan<ShadowData> box_shadow_span() const
        {
            static_assert(sizeof(ShadowData) == sizeof(ComputedValuesFFI::ComputedShadow));
            static_assert(alignof(ShadowData) == alignof(ComputedValuesFFI::ComputedShadow));
            return { reinterpret_cast<ShadowData const*>(box_shadows.pointer), box_shadows.length };
        }
        Clip clip_value() const
        {
            if (!clip_is_rect)
                return Clip::make_auto();
            auto edge = [](ComputedValuesFFI::ComputedClipEdge const& value) {
                if (value.is_auto)
                    return LengthOrAuto::make_auto();
                return LengthOrAuto { Length { value.value, static_cast<LengthUnit>(value.unit) } };
            };
            return Clip { EdgeRect {
                edge(clip_edges[0]),
                edge(clip_edges[1]),
                edge(clip_edges[2]),
                edge(clip_edges[3]),
            } };
        }

        bool operator==(EffectsValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
    };

    struct MaskValues : ComputedValuesFFI::MaskValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::MaskValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::Mask;

        Optional<MaskReference> mask_value() const;
        MaskType mask_type_value() const;
        RefPtr<AbstractImageStyleValue const> mask_image_value() const;
        Vector<BackgroundLayerData> mask_layers_value() const;
        Vector<Position> mask_positions_value() const;
        Optional<ClipPathReference> clip_path_value() const;

        bool operator==(MaskValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
    };

    struct TextResetValues : ComputedValuesFFI::TextResetValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::TextResetValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::TextReset;

        ReadonlySpan<TextDecorationLine> decoration_lines() const
        {
            static_assert(sizeof(TextDecorationLine) == sizeof(u8));
            return { reinterpret_cast<TextDecorationLine const*>(text_decoration_lines.pointer), text_decoration_lines.length };
        }

        TextDecorationThickness decoration_thickness() const
        {
            if (text_decoration_thickness_kind == 0)
                return TextDecorationThickness { TextDecorationThickness::Auto {} };
            if (text_decoration_thickness_kind == 1)
                return TextDecorationThickness { TextDecorationThickness::FromFont {} };
            return TextDecorationThickness { LengthPercentage::view(text_decoration_thickness) };
        }

        WhiteSpaceTrimData white_space_trim() const
        {
            return {
                .discard_before = white_space_trim_discard_before,
                .discard_after = white_space_trim_discard_after,
                .discard_inner = white_space_trim_discard_inner,
            };
        }

        bool operator==(TextResetValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
    };

    struct ContentValues : ComputedValuesFFI::ContentValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::ContentValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::Content;

        ComputedContentData computed_content_value() const;
        Vector<CounterData, 0> counter_increment_value() const;
        Vector<CounterData, 0> counter_reset_value() const;
        Vector<CounterData, 0> counter_set_value() const;

        bool operator==(ContentValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
    };

    struct TransformValues : public ComputedValuesFFI::TransformValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::TransformValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::Transform;

        static RefPtr<StyleValue const> style_value(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
        {
            if (!handle.pointer)
                return nullptr;
            return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                static_cast<StyleValueFFI::StyleValueData const*>(handle.pointer)));
        }

        bool has_transformations() const { return transformations.pointer; }
        template<typename Callback>
        void for_each_transformation(Callback callback) const
        {
            auto value = style_value(transformations);
            if (!value)
                return;
            for (auto const& transformation : transformations_for_style_value(*value))
                callback(*transformation);
        }

        bool has_resolved_transforms() const { return resolved_transforms.length != 0; }
        template<typename Callback>
        void for_each_resolved_transform(Callback callback) const
        {
            for (size_t index = 0; index < resolved_transforms.length; ++index) {
                auto const& entry = resolved_transforms.pointer[index];
                if (entry.is_translate) {
                    callback(ResolvedTransform { ResolvedTransform::Translate {
                        .x = { .px = entry.x_px, .percentage_value = style_value(entry.x_percentage) },
                        .y = { .px = entry.y_px, .percentage_value = style_value(entry.y_percentage) },
                        .z = entry.z_px,
                    } });
                    continue;
                }
                auto const& matrix = entry.matrix;
                callback(ResolvedTransform { FloatMatrix4x4(
                    matrix[0], matrix[1], matrix[2], matrix[3],
                    matrix[4], matrix[5], matrix[6], matrix[7],
                    matrix[8], matrix[9], matrix[10], matrix[11],
                    matrix[12], matrix[13], matrix[14], matrix[15]) });
            }
        }

        TransformBox transform_box_value() const { return static_cast<TransformBox>(transform_box); }
        TransformOrigin transform_origin_value() const
        {
            return {
                LengthPercentage::view(transform_origin_x),
                LengthPercentage::view(transform_origin_y),
                LengthPercentage::view(transform_origin_z),
            };
        }
        TransformStyle transform_style_value() const { return static_cast<TransformStyle>(transform_style); }
        BackfaceVisibility backface_visibility_value() const { return static_cast<BackfaceVisibility>(backface_visibility); }
        RefPtr<TransformationStyleValue const> rotate_value() const { return transformation_value(rotate); }
        RefPtr<TransformationStyleValue const> translate_value() const { return transformation_value(translate); }
        RefPtr<TransformationStyleValue const> scale_value() const { return transformation_value(scale); }
        static void set_transformation(ComputedValuesFFI::ComputedStyleValueHandle& handle, TransformationStyleValue const* value)
        {
            StyleValueFFI::rust_style_value_release(static_cast<StyleValueFFI::StyleValueData const*>(handle.pointer));
            handle.pointer = value ? StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()) : nullptr;
        }
        Optional<CSSPixels> perspective_value() const
        {
            if (!has_perspective)
                return {};
            return CSSPixels::from_raw(perspective_px);
        }
        Position perspective_origin_value() const
        {
            return {
                .offset_x = LengthPercentage::view(perspective_origin_x),
                .offset_y = LengthPercentage::view(perspective_origin_y),
            };
        }

        bool operator==(TransformValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }

    private:
        static RefPtr<TransformationStyleValue const> transformation_value(ComputedValuesFFI::ComputedStyleValueHandle const& handle)
        {
            auto value = style_value(handle);
            if (!value)
                return nullptr;
            return value->as_transformation();
        }
    };

    struct BackgroundValues : ComputedValuesFFI::BackgroundValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::BackgroundValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::Background;

        Color background_color_value() const { return Color::from_bgra(background_color); }
        BackgroundBox background_color_clip_value() const { return static_cast<BackgroundBox>(background_color_clip); }
        Vector<BackgroundLayerData> background_layers_value() const;

        bool operator==(BackgroundValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
    };

    struct BorderValues : ComputedValuesFFI::BorderValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::BorderValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::Border;

        BorderData const& border_left_value() const { return reinterpret_cast<BorderData const&>(border_left); }
        BorderData const& border_top_value() const { return reinterpret_cast<BorderData const&>(border_top); }
        BorderData const& border_right_value() const { return reinterpret_cast<BorderData const&>(border_right); }
        BorderData const& border_bottom_value() const { return reinterpret_cast<BorderData const&>(border_bottom); }
        CSSPixels border_left_computed_width_value() const { return CSSPixels::from_raw(border_left_computed_width); }
        CSSPixels border_top_computed_width_value() const { return CSSPixels::from_raw(border_top_computed_width); }
        CSSPixels border_right_computed_width_value() const { return CSSPixels::from_raw(border_right_computed_width); }
        CSSPixels border_bottom_computed_width_value() const { return CSSPixels::from_raw(border_bottom_computed_width); }
        BorderRadiusData border_bottom_left_radius_value() const;
        BorderRadiusData border_bottom_right_radius_value() const;
        BorderRadiusData border_top_left_radius_value() const;
        BorderRadiusData border_top_right_radius_value() const;
        bool has_noninitial_border_radii_value() const;
        BorderImageData border_image_value() const;

        bool operator==(BorderValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
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

    struct MiscResetValues : ComputedValuesFFI::MiscResetValues {
        static constexpr size_t style_group_index = to_underlying(StyleGroupIndex::MiscResetValues);
        static constexpr auto style_group_lifecycle = ComputedValuesFFI::StyleGroupLifecycle::MiscReset;

        RefPtr<StyleValue const> outline_offset_style_value_value() const;
        OverflowClipMarginData overflow_clip_margin_value() const;
        Position object_position_value() const;
        Optional<Utf16FlyString> view_transition_name_value() const;
        TouchActionData touch_action_value() const;
        ShapeOutsideData shape_outside_value() const;
        WillChange will_change_value() const;

        bool operator==(MiscResetValues const& other) const
        {
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
        }
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
            return ComputedValuesFFI::rust_style_group_payloads_equal(style_group_index, this, &other);
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

    // Retains `table` (releasing any table held before) and points the value span at it.
    void adopt_computed_longhand_table(void const* table);
    // Takes `previous`'s table when every slot holds an equal value, so the next publication
    // interns the same pointers and keeps the style-record identity.
    void adopt_identical_computed_longhand_table(ComputedValues const& previous) const;
    void clear_computed_longhand_table();
    // Takes `other`'s table by reference count, or materializes an owned table from `other`'s
    // borrowed record span, so the copy never outlives its source's storage.
    void copy_computed_longhand_table_from(ComputedValues const& other);
    // For the inherited-group swap: builds this style's table from `old_values`'s slots with
    // every inherited-by-default longhand replaced by `inherited_source`'s value, mirroring
    // the group replacement, so the swapped style stays a complete inheritance source.
    void adopt_swapped_computed_longhand_table(ComputedValues const& old_values, ComputedValues const& inherited_source);

    NonInheritedValues m_noninherited;
    AK::FixedBitmap<number_of_longhand_properties> m_property_important { false };
    AK::FixedBitmap<number_of_longhand_properties> m_property_inherited { false };
    HashMap<PropertyID, NonnullRefPtr<StyleValue const>> m_inheritance_dependent_specified_values;
    mutable HashMap<PropertyID, NonnullRefPtr<StyleValue const>> m_style_value_cache;
    RefPtr<StyleValue const> m_raw_cascaded_font_size;
    StyleValueFFI::StyleValueData const* m_borrowed_raw_cascaded_font_size { nullptr };
    ReadonlySpan<StyleEngineFFI::FfiInheritanceDependentValue const> m_borrowed_inheritance_dependent_values;
    // The drive's frozen computed longhand table, retained when this style owns a reference;
    // null for borrowed style-record views and for styles built without a drive.
    void const* m_computed_longhand_table { nullptr };
    // One stored data pointer per longhand (null where the drive stored none): the owned table's
    // span, or the span borrowed from the style record's interned table. Empty without a table.
    ReadonlySpan<void const*> m_longhand_values;
    RefPtr<ComputedValues const> m_base_values;
    ComputedValues const* m_borrowed_base_values { nullptr };
    RefPtr<AnimatedProperties const> m_animated_properties;
    u64 m_pseudo_element_styles { 0 };
    bool m_depends_on_viewport_metrics { false };
    bool m_font_metrics_depend_on_viewport_metrics { false };
    bool m_in_display_none_subtree { false };
    bool m_is_style_record_view { false };
};

// A synchronous, allocation-free compatibility surface over the payloads of
// one authoritative StyleRecord. It owns no group or metadata payload.
class WEB_API ComputedStyleRecordView {
    AK_MAKE_NONCOPYABLE(ComputedStyleRecordView);
    AK_MAKE_NONMOVABLE(ComputedStyleRecordView);

public:
    ComputedStyleRecordView() = default;
    ComputedStyleRecordView(StyleEngineFFI::FfiStyleRecordView const&, StyleComputer const&, StyleRecordID);
    ~ComputedStyleRecordView();

    void retain_across_style_record_publication();

    explicit operator bool() const { return m_present; }
    ComputedValues const* operator->() const
    {
        if (!m_present)
            return nullptr;
        return m_retained_values ? m_retained_values.ptr() : &m_values;
    }
    ComputedValues const& operator*() const
    {
        VERIFY(m_present);
        return m_retained_values ? *m_retained_values : m_values;
    }

private:
    ComputedValues m_base_values { ComputedValues::BorrowedStyleRecord::Yes };
    ComputedValues m_values { ComputedValues::BorrowedStyleRecord::Yes };
    RefPtr<ComputedValues const> m_retained_values;
    GC::Ptr<StyleComputer const> m_style_computer;
    StyleRecordID m_style_record_identity;
    bool m_present { false };
};

// The input to layout-node construction is either an authoritative style
// record for a DOM style target or an owned style for an anonymous box.
class LayoutStyle {
public:
    LayoutStyle() = default;
    LayoutStyle(StyleRecordID style_record_identity)
        : m_style_record_identity(style_record_identity)
    {
        VERIFY(style_record_identity);
    }
    LayoutStyle(NonnullRefPtr<ComputedValues const> values)
        : m_values(move(values))
    {
    }
    LayoutStyle(RefPtr<ComputedValues const> values)
        : m_values(move(values))
    {
    }

    explicit operator bool() const { return !!m_style_record_identity || m_values; }
    [[nodiscard]] StyleRecordID style_record_identity() const { return m_style_record_identity; }
    [[nodiscard]] RefPtr<ComputedValues const> const& values() const { return m_values; }

private:
    RefPtr<ComputedValues const> m_values;
    StyleRecordID m_style_record_identity;
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
    void set_property_flag_bitmaps(ReadonlyBytes importance, ReadonlyBytes inheritance)
    {
        m_values.m_property_important.copy_from(importance);
        m_values.m_property_inherited.copy_from(inheritance);
    }
    void set_depends_on_viewport_metrics(bool value) { m_values.m_depends_on_viewport_metrics = value; }
    void set_font_metrics_depend_on_viewport_metrics(bool value) { m_values.m_font_metrics_depend_on_viewport_metrics = value; }
    void set_in_display_none_subtree(bool value) { m_values.m_in_display_none_subtree = value; }
    void set_pseudo_element_styles(u64 value) { m_values.m_pseudo_element_styles = value; }
    void set_inheritance_dependent_specified_values(HashMap<PropertyID, NonnullRefPtr<StyleValue const>> value) { m_values.m_inheritance_dependent_specified_values = move(value); }
    void set_raw_cascaded_font_size(RefPtr<StyleValue const> value) { m_values.m_raw_cascaded_font_size = move(value); }
    void set_computed_longhand_table(void const* table) { m_values.adopt_computed_longhand_table(table); }
    void set_base_values(NonnullRefPtr<ComputedValues const> value)
    {
        m_values.m_base_values = move(value);
        m_values.m_borrowed_base_values = nullptr;
    }
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
    void adopt_font_group(void* payload) { m_values.m_inherited.font.adopt(payload); }
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
        if (m_values.m_inherited.text->color_value() == color)
            return;
        m_values.m_inherited.text.access().color = color.value();
    }
    void set_color_scheme(PreferredColorScheme color_scheme)
    {
        if (m_values.m_inherited.ui->color_scheme_value() == color_scheme)
            return;
        m_values.m_inherited.ui.access().color_scheme = to_underlying(color_scheme);
    }
    void set_clip(Clip const& clip)
    {
        if (m_values.clip() == clip)
            return;
        auto& effects = m_values.m_noninherited.effects.access();
        effects.clip_is_rect = clip.is_rect();
        if (!clip.is_rect())
            return;
        auto rect = clip.to_rect();
        auto set_edge = [](auto& output, LengthOrAuto const& input) {
            output.is_auto = input.is_auto();
            output.value = input.is_auto() ? 0 : input.length().raw_value();
            output.unit = input.is_auto() ? to_underlying(LengthUnit::Px) : to_underlying(input.length().unit());
        };
        set_edge(effects.clip_edges[0], rect.top_edge);
        set_edge(effects.clip_edges[1], rect.right_edge);
        set_edge(effects.clip_edges[2], rect.bottom_edge);
        set_edge(effects.clip_edges[3], rect.left_edge);
    }
    void set_content_visibility(ContentVisibility content_visibility)
    {
        if (m_values.m_inherited.box->content_visibility == to_underlying(content_visibility))
            return;
        m_values.m_inherited.box.access().content_visibility = to_underlying(content_visibility);
    }
    void set_image_rendering(ImageRendering value)
    {
        if (m_values.m_inherited.box->image_rendering == to_underlying(value))
            return;
        m_values.m_inherited.box.access().image_rendering = to_underlying(value);
    }
    void set_background_color(Color color)
    {
        if (m_values.m_noninherited.background->background_color_value() == color)
            return;
        m_values.m_noninherited.background.access().background_color = color.value();
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
    void set_text_align(TextAlign text_align)
    {
        if (m_values.m_inherited.text->text_align_value() == text_align)
            return;
        m_values.m_inherited.text.access().text_align = to_underlying(text_align);
    }
    void set_text_decoration_line(Vector<TextDecorationLine> value)
    {
        if (m_values.text_decoration_line() == value.span())
            return;
        Vector<u8> lines;
        lines.ensure_capacity(value.size());
        for (auto line : value)
            lines.unchecked_append(to_underlying(line));
        auto& text_reset = m_values.m_noninherited.text_reset.access();
        ComputedValuesFFI::rust_text_reset_set_decoration_lines(&text_reset, lines.data(), lines.size());
    }
    void set_text_decoration_thickness(TextDecorationThickness value)
    {
        if (m_values.text_decoration_thickness() == value)
            return;
        auto& text_reset = m_values.m_noninherited.text_reset.access();
        value.value.visit(
            [&](TextDecorationThickness::Auto const&) {
                ComputedValuesFFI::rust_text_reset_set_decoration_thickness(&text_reset, 0, nullptr);
            },
            [&](TextDecorationThickness::FromFont const&) {
                ComputedValuesFFI::rust_text_reset_set_decoration_thickness(&text_reset, 1, nullptr);
            },
            [&](LengthPercentage& length_percentage) {
                ComputedValuesFFI::rust_text_reset_set_decoration_thickness(&text_reset, 2, length_percentage.leak_data());
            });
    }
    void set_text_decoration_style(TextDecorationStyle value)
    {
        if (m_values.text_decoration_style() == value)
            return;
        m_values.m_noninherited.text_reset.access().text_decoration_style = to_underlying(value);
    }
    void set_text_decoration_color(Color value)
    {
        if (m_values.text_decoration_color() == value)
            return;
        m_values.m_noninherited.text_reset.access().text_decoration_color = value.value();
    }
    void set_position(Positioning position)
    {
        if (m_values.m_noninherited.box->position == to_underlying(position))
            return;
        m_values.m_noninherited.box.access().position = to_underlying(position);
    }
    // The surround payload retains the position-anchor style value for the
    // layout engine's anchor lookup; an empty handle means no name.
    void set_position_anchor(PositionAnchor value)
    {
        if (m_values.m_noninherited.anchor->position_anchor_value() == value)
            return;
        auto& surround = m_values.m_noninherited.surround.access();
        ComputedValuesFFI::rust_surround_set_position_anchor(&surround, value.name.has_value() ? value.name->to_raw_leaked() : 0);
        auto& anchor = m_values.m_noninherited.anchor.access();
        ComputedValuesFFI::rust_anchor_set_position_anchor(
            &anchor,
            to_underlying(value.type),
            value.name.has_value() ? value.name->to_raw_leaked() : 0);
    }
    void set_letter_spacing(CSSPixels value)
    {
        if (m_values.m_inherited.text->letter_spacing_value() == value)
            return;
        m_values.m_inherited.text.access().letter_spacing = value.raw_value();
    }
    void set_width(Size value) { set_size(&ComputedValuesFFI::SizingValues::width, move(value)); }
    void set_height(Size value) { set_size(&ComputedValuesFFI::SizingValues::height, move(value)); }
    void set_min_height(Size value) { set_size(&ComputedValuesFFI::SizingValues::min_height, move(value)); }
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
    void set_border_top_color(Color value)
    {
        if (m_values.m_noninherited.border->border_top_value().color == value)
            return;
        m_values.m_noninherited.border.access().border_top.color = value.value();
    }
    void set_border_right_color(Color value)
    {
        if (m_values.m_noninherited.border->border_right_value().color == value)
            return;
        m_values.m_noninherited.border.access().border_right.color = value.value();
    }
    void set_border_bottom_color(Color value)
    {
        if (m_values.m_noninherited.border->border_bottom_value().color == value)
            return;
        m_values.m_noninherited.border.access().border_bottom.color = value.value();
    }
    void set_border_left_color(Color value)
    {
        if (m_values.m_noninherited.border->border_left_value().color == value)
            return;
        m_values.m_noninherited.border.access().border_left.color = value.value();
    }
    void set_flex_direction(FlexDirection value)
    {
        if (m_values.flex_direction() == value)
            return;
        m_values.m_noninherited.alignment.access().flex_direction = to_underlying(value);
    }
    void set_order(i32 value)
    {
        if (m_values.m_noninherited.alignment->order == value)
            return;
        m_values.m_noninherited.alignment.access().order = value;
    }
    void set_align_self(AlignSelf value)
    {
        if (m_values.align_self() == value)
            return;
        m_values.m_noninherited.alignment.access().align_self = to_underlying(value);
    }
    void set_justify_content(JustifyContent value)
    {
        if (m_values.justify_content() == value)
            return;
        m_values.m_noninherited.alignment.access().justify_content = to_underlying(value);
    }
    void set_justify_self(JustifySelf value)
    {
        if (m_values.justify_self() == value)
            return;
        m_values.m_noninherited.alignment.access().justify_self = to_underlying(value);
    }
    void set_rotate(RefPtr<TransformationStyleValue const> value)
    {
        if (m_values.rotate() == value)
            return;
        TransformValues::set_transformation(m_values.m_noninherited.transform.access().rotate, value.ptr());
    }
    void set_scale(RefPtr<TransformationStyleValue const> value)
    {
        if (m_values.scale() == value)
            return;
        TransformValues::set_transformation(m_values.m_noninherited.transform.access().scale, value.ptr());
    }
    void set_translate(RefPtr<TransformationStyleValue const> value)
    {
        if (m_values.translate() == value)
            return;
        TransformValues::set_transformation(m_values.m_noninherited.transform.access().translate, value.ptr());
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
    void copy_grid_placements_from(ComputedValues const& source)
    {
        copy_grid_placements_from(*source.m_noninherited.grid);
    }
    void copy_grid_placements_from(GridValues const& source)
    {
        auto const* source_grid = static_cast<ComputedValuesFFI::GridValues const*>(&source);
        auto const* current_grid = static_cast<ComputedValuesFFI::GridValues const*>(m_values.m_noninherited.grid.operator->());
        if (ComputedValuesFFI::rust_grid_values_placements_equal(source_grid, current_grid))
            return;
        ComputedValuesFFI::rust_grid_values_copy_placements(
            source_grid,
            &m_values.m_noninherited.grid.access());
    }
    void reset_grid_placements_to_auto()
    {
        // Every producer writes auto placements in canonical form, so a kind
        // check alone detects the already-auto case without cloning.
        auto placement_is_auto = [](ComputedValuesFFI::ComputedGridPlacement const& placement) {
            return placement.kind == to_underlying(ComputedValuesFFI::ComputedGridPlacementKind::Auto);
        };
        auto const& grid = *m_values.m_noninherited.grid;
        if (placement_is_auto(grid.column_start) && placement_is_auto(grid.column_end)
            && placement_is_auto(grid.row_start) && placement_is_auto(grid.row_end))
            return;
        ComputedValuesFFI::rust_grid_values_reset_placements_to_auto(&m_values.m_noninherited.grid.access());
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
    void set_direction(Direction value)
    {
        if (m_values.m_inherited.box->direction == to_underlying(value))
            return;
        m_values.m_inherited.box.access().direction = to_underlying(value);
    }
    void set_writing_mode(WritingMode value)
    {
        if (m_values.m_inherited.box->writing_mode == to_underlying(value))
            return;
        m_values.m_inherited.box.access().writing_mode = to_underlying(value);
    }
    void set_outline_color(Color value)
    {
        if (m_values.m_noninherited.misc->outline_color == value.value())
            return;
        m_values.m_noninherited.misc.access().outline_color = value.value();
    }
    void set_scrollbar_width(ScrollbarWidth value)
    {
        if (m_values.m_noninherited.misc->scrollbar_width == to_underlying(value))
            return;
        m_values.m_noninherited.misc.access().scrollbar_width = to_underlying(value);
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
        for (auto const& entry : values.m_borrowed_inheritance_dependent_values) {
            auto const* data = static_cast<StyleValueFFI::StyleValueData const*>(entry.value);
            m_values->m_inheritance_dependent_specified_values.set(
                static_cast<PropertyID>(entry.property),
                StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(data)));
        }
        m_values->m_raw_cascaded_font_size = values.raw_cascaded_font_size();
        m_values->copy_computed_longhand_table_from(values);
        if (values.m_borrowed_base_values)
            m_values->m_base_values = Builder { *values.m_borrowed_base_values }.build();
        else
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

    // A copy of `values` whose inherited half is `inherited_source`'s, group references swapped
    // rather than payloads rebuilt. Only correct when `values` takes every inherited property by
    // standard inheritance (see property_inheritance_is_standard()).
    static Builder create_with_inherited_style_replaced(ComputedValues const& values, ComputedValues const& inherited_source)
    {
        Builder builder { values };
        builder.m_values->m_inherited = inherited_source.m_inherited;
        // The copied longhand table still names the old parent's inherited values;
        // replace those slots with the new parent's, like the groups above.
        builder.m_values->adopt_swapped_computed_longhand_table(values, inherited_source);
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
