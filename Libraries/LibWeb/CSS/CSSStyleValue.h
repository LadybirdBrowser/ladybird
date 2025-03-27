/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericShorthands.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibGfx/Color.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/Keyword.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

template<typename T>
struct ValueComparingNonnullRefPtr : public NonnullRefPtr<T> {
    using NonnullRefPtr<T>::NonnullRefPtr;

    ValueComparingNonnullRefPtr(NonnullRefPtr<T> const& other)
        : NonnullRefPtr<T>(other)
    {
    }

    ValueComparingNonnullRefPtr(NonnullRefPtr<T>&& other)
        : NonnullRefPtr<T>(move(other))
    {
    }

    bool operator==(ValueComparingNonnullRefPtr const& other) const
    {
        return this->ptr() == other.ptr() || this->ptr()->equals(*other);
    }

private:
    using NonnullRefPtr<T>::operator==;
};

template<typename T>
struct ValueComparingRefPtr : public RefPtr<T> {
    using RefPtr<T>::RefPtr;

    ValueComparingRefPtr(RefPtr<T> const& other)
        : RefPtr<T>(other)
    {
    }

    ValueComparingRefPtr(RefPtr<T>&& other)
        : RefPtr<T>(move(other))
    {
    }

    template<typename U>
    bool operator==(ValueComparingNonnullRefPtr<U> const& other) const
    {
        return this->ptr() == other.ptr() || (this->ptr() && this->ptr()->equals(*other));
    }

    bool operator==(ValueComparingRefPtr const& other) const
    {
        return this->ptr() == other.ptr() || (this->ptr() && other.ptr() && this->ptr()->equals(*other));
    }

private:
    using RefPtr<T>::operator==;
};

using StyleValueVector = Vector<ValueComparingNonnullRefPtr<CSSStyleValue const>>;

// https://drafts.css-houdini.org/css-typed-om-1/#cssstylevalue
class CSSStyleValue : public RefCounted<CSSStyleValue> {
public:
    virtual ~CSSStyleValue() = default;

    enum class Type {
        Angle,
        BackgroundRepeat,
        BackgroundSize,
        BasicShape,
        BorderRadius,
        Calculated,
        Color,
        ColorScheme,
        ConicGradient,
        Content,
        Counter,
        CounterDefinitions,
        Cursor,
        CustomIdent,
        Display,
        Easing,
        Edge,
        FilterValueList,
        FitContent,
        Flex,
        FontVariant,
        Frequency,
        GridAutoFlow,
        GridTemplateArea,
        GridTrackPlacement,
        GridTrackSizeList,
        Image,
        Integer,
        Keyword,
        Length,
        LinearGradient,
        MathDepth,
        Number,
        OpenTypeTagged,
        Percentage,
        Position,
        RadialGradient,
        Ratio,
        Rect,
        Resolution,
        ScrollbarGutter,
        Shadow,
        Shorthand,
        String,
        Time,
        Transformation,
        Transition,
        UnicodeRange,
        Unresolved,
        URL,
        ValueList,
    };

    Type type() const { return m_type; }

    bool is_abstract_image() const
    {
        return AK::first_is_one_of(type(), Type::Image, Type::LinearGradient, Type::ConicGradient, Type::RadialGradient);
    }
    AbstractImageStyleValue const& as_abstract_image() const;
    AbstractImageStyleValue& as_abstract_image() { return const_cast<AbstractImageStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_abstract_image()); }

    bool is_angle() const { return type() == Type::Angle; }
    AngleStyleValue const& as_angle() const;
    AngleStyleValue& as_angle() { return const_cast<AngleStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_angle()); }

    bool is_background_repeat() const { return type() == Type::BackgroundRepeat; }
    BackgroundRepeatStyleValue const& as_background_repeat() const;
    BackgroundRepeatStyleValue& as_background_repeat() { return const_cast<BackgroundRepeatStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_background_repeat()); }

    bool is_background_size() const { return type() == Type::BackgroundSize; }
    BackgroundSizeStyleValue const& as_background_size() const;
    BackgroundSizeStyleValue& as_background_size() { return const_cast<BackgroundSizeStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_background_size()); }

    bool is_basic_shape() const { return type() == Type::BasicShape; }
    BasicShapeStyleValue const& as_basic_shape() const;
    BasicShapeStyleValue& as_basic_shape() { return const_cast<BasicShapeStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_basic_shape()); }

    bool is_border_radius() const { return type() == Type::BorderRadius; }
    BorderRadiusStyleValue const& as_border_radius() const;
    BorderRadiusStyleValue& as_border_radius() { return const_cast<BorderRadiusStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_border_radius()); }

    bool is_calculated() const { return type() == Type::Calculated; }
    CalculatedStyleValue const& as_calculated() const;
    CalculatedStyleValue& as_calculated() { return const_cast<CalculatedStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_calculated()); }

    bool is_color() const { return type() == Type::Color; }
    CSSColorValue const& as_color() const;
    CSSColorValue& as_color() { return const_cast<CSSColorValue&>(const_cast<CSSStyleValue const&>(*this).as_color()); }

    bool is_color_scheme() const { return type() == Type::ColorScheme; }
    ColorSchemeStyleValue const& as_color_scheme() const;
    ColorSchemeStyleValue& as_color_scheme() { return const_cast<ColorSchemeStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_color_scheme()); }

    bool is_conic_gradient() const { return type() == Type::ConicGradient; }
    ConicGradientStyleValue const& as_conic_gradient() const;
    ConicGradientStyleValue& as_conic_gradient() { return const_cast<ConicGradientStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_conic_gradient()); }

    bool is_content() const { return type() == Type::Content; }
    ContentStyleValue const& as_content() const;
    ContentStyleValue& as_content() { return const_cast<ContentStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_content()); }

    bool is_counter() const { return type() == Type::Counter; }
    CounterStyleValue const& as_counter() const;
    CounterStyleValue& as_counter() { return const_cast<CounterStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_counter()); }

    bool is_counter_definitions() const { return type() == Type::CounterDefinitions; }
    CounterDefinitionsStyleValue const& as_counter_definitions() const;
    CounterDefinitionsStyleValue& as_counter_definitions() { return const_cast<CounterDefinitionsStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_counter_definitions()); }

    bool is_cursor() const { return type() == Type::Cursor; }
    CursorStyleValue const& as_cursor() const;
    CursorStyleValue& as_cursor() { return const_cast<CursorStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_cursor()); }

    bool is_custom_ident() const { return type() == Type::CustomIdent; }
    CustomIdentStyleValue const& as_custom_ident() const;
    CustomIdentStyleValue& as_custom_ident() { return const_cast<CustomIdentStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_custom_ident()); }

    bool is_display() const { return type() == Type::Display; }
    DisplayStyleValue const& as_display() const;
    DisplayStyleValue& as_display() { return const_cast<DisplayStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_display()); }

    bool is_easing() const { return type() == Type::Easing; }
    EasingStyleValue const& as_easing() const;
    EasingStyleValue& as_easing() { return const_cast<EasingStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_easing()); }

    bool is_edge() const { return type() == Type::Edge; }
    EdgeStyleValue const& as_edge() const;
    EdgeStyleValue& as_edge() { return const_cast<EdgeStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_edge()); }

    bool is_filter_value_list() const { return type() == Type::FilterValueList; }
    FilterValueListStyleValue const& as_filter_value_list() const;
    FilterValueListStyleValue& as_filter_value_list() { return const_cast<FilterValueListStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_filter_value_list()); }

    [[nodiscard]] bool is_fit_content() const { return type() == Type::FitContent; }
    [[nodiscard]] FitContentStyleValue const& as_fit_content() const;
    [[nodiscard]] FitContentStyleValue& as_fit_content() { return const_cast<FitContentStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_fit_content()); }

    bool is_flex() const { return type() == Type::Flex; }
    FlexStyleValue const& as_flex() const;
    FlexStyleValue& as_flex() { return const_cast<FlexStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_flex()); }

    bool is_frequency() const { return type() == Type::Frequency; }
    FrequencyStyleValue const& as_frequency() const;
    FrequencyStyleValue& as_frequency() { return const_cast<FrequencyStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_frequency()); }

    bool is_grid_auto_flow() const { return type() == Type::GridAutoFlow; }
    GridAutoFlowStyleValue const& as_grid_auto_flow() const;
    GridAutoFlowStyleValue& as_grid_auto_flow() { return const_cast<GridAutoFlowStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_grid_auto_flow()); }

    bool is_grid_template_area() const { return type() == Type::GridTemplateArea; }
    GridTemplateAreaStyleValue const& as_grid_template_area() const;
    GridTemplateAreaStyleValue& as_grid_template_area() { return const_cast<GridTemplateAreaStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_grid_template_area()); }

    bool is_grid_track_placement() const { return type() == Type::GridTrackPlacement; }
    GridTrackPlacementStyleValue const& as_grid_track_placement() const;
    GridTrackPlacementStyleValue& as_grid_track_placement() { return const_cast<GridTrackPlacementStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_grid_track_placement()); }

    bool is_grid_track_size_list() const { return type() == Type::GridTrackSizeList; }
    GridTrackSizeListStyleValue const& as_grid_track_size_list() const;
    GridTrackSizeListStyleValue& as_grid_track_size_list() { return const_cast<GridTrackSizeListStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_grid_track_size_list()); }

    bool is_image() const { return type() == Type::Image; }
    ImageStyleValue const& as_image() const;
    ImageStyleValue& as_image() { return const_cast<ImageStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_image()); }

    bool is_integer() const { return type() == Type::Integer; }
    IntegerStyleValue const& as_integer() const;
    IntegerStyleValue& as_integer() { return const_cast<IntegerStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_integer()); }

    bool is_keyword() const { return type() == Type::Keyword; }
    CSSKeywordValue const& as_keyword() const;
    CSSKeywordValue& as_keyword() { return const_cast<CSSKeywordValue&>(const_cast<CSSStyleValue const&>(*this).as_keyword()); }

    bool is_length() const { return type() == Type::Length; }
    LengthStyleValue const& as_length() const;
    LengthStyleValue& as_length() { return const_cast<LengthStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_length()); }

    bool is_linear_gradient() const { return type() == Type::LinearGradient; }
    LinearGradientStyleValue const& as_linear_gradient() const;
    LinearGradientStyleValue& as_linear_gradient() { return const_cast<LinearGradientStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_linear_gradient()); }

    bool is_math_depth() const { return type() == Type::MathDepth; }
    MathDepthStyleValue const& as_math_depth() const;
    MathDepthStyleValue& as_math_depth() { return const_cast<MathDepthStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_math_depth()); }

    bool is_number() const { return type() == Type::Number; }
    NumberStyleValue const& as_number() const;
    NumberStyleValue& as_number() { return const_cast<NumberStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_number()); }

    bool is_open_type_tagged() const { return type() == Type::OpenTypeTagged; }
    OpenTypeTaggedStyleValue const& as_open_type_tagged() const;
    OpenTypeTaggedStyleValue& as_open_type_tagged() { return const_cast<OpenTypeTaggedStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_open_type_tagged()); }

    bool is_percentage() const { return type() == Type::Percentage; }
    PercentageStyleValue const& as_percentage() const;
    PercentageStyleValue& as_percentage() { return const_cast<PercentageStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_percentage()); }

    bool is_position() const { return type() == Type::Position; }
    PositionStyleValue const& as_position() const;

    PositionStyleValue& as_position() { return const_cast<PositionStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_position()); }
    bool is_radial_gradient() const { return type() == Type::RadialGradient; }
    RadialGradientStyleValue const& as_radial_gradient() const;
    RadialGradientStyleValue& as_radial_gradient() { return const_cast<RadialGradientStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_radial_gradient()); }

    bool is_ratio() const { return type() == Type::Ratio; }
    RatioStyleValue const& as_ratio() const;
    RatioStyleValue& as_ratio() { return const_cast<RatioStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_ratio()); }

    bool is_rect() const { return type() == Type::Rect; }
    RectStyleValue const& as_rect() const;
    RectStyleValue& as_rect() { return const_cast<RectStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_rect()); }

    bool is_resolution() const { return type() == Type::Resolution; }
    ResolutionStyleValue const& as_resolution() const;
    ResolutionStyleValue& as_resolution() { return const_cast<ResolutionStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_resolution()); }

    bool is_scrollbar_gutter() const { return type() == Type::ScrollbarGutter; }
    ScrollbarGutterStyleValue const& as_scrollbar_gutter() const;
    ScrollbarGutterStyleValue& as_scrollbar_gutter() { return const_cast<ScrollbarGutterStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_scrollbar_gutter()); }

    bool is_shadow() const { return type() == Type::Shadow; }
    ShadowStyleValue const& as_shadow() const;
    ShadowStyleValue& as_shadow() { return const_cast<ShadowStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_shadow()); }

    bool is_shorthand() const { return type() == Type::Shorthand; }
    ShorthandStyleValue const& as_shorthand() const;
    ShorthandStyleValue& as_shorthand() { return const_cast<ShorthandStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_shorthand()); }

    bool is_string() const { return type() == Type::String; }
    StringStyleValue const& as_string() const;
    StringStyleValue& as_string() { return const_cast<StringStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_string()); }

    bool is_time() const { return type() == Type::Time; }
    TimeStyleValue const& as_time() const;
    TimeStyleValue& as_time() { return const_cast<TimeStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_time()); }

    bool is_transformation() const { return type() == Type::Transformation; }
    TransformationStyleValue const& as_transformation() const;
    TransformationStyleValue& as_transformation() { return const_cast<TransformationStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_transformation()); }

    bool is_transition() const { return type() == Type::Transition; }
    TransitionStyleValue const& as_transition() const;
    TransitionStyleValue& as_transition() { return const_cast<TransitionStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_transition()); }

    bool is_unicode_range() const { return type() == Type::UnicodeRange; }
    UnicodeRangeStyleValue const& as_unicode_range() const;
    UnicodeRangeStyleValue& as_unicode_range() { return const_cast<UnicodeRangeStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_unicode_range()); }

    bool is_unresolved() const { return type() == Type::Unresolved; }
    UnresolvedStyleValue const& as_unresolved() const;
    UnresolvedStyleValue& as_unresolved() { return const_cast<UnresolvedStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_unresolved()); }

    bool is_url() const { return type() == Type::URL; }
    URLStyleValue const& as_url() const;
    URLStyleValue& as_url() { return const_cast<URLStyleValue&>(const_cast<CSSStyleValue const&>(*this).as_url()); }

    bool is_value_list() const { return type() == Type::ValueList; }
    StyleValueList const& as_value_list() const;
    StyleValueList& as_value_list() { return const_cast<StyleValueList&>(const_cast<CSSStyleValue const&>(*this).as_value_list()); }

    // https://www.w3.org/TR/css-values-4/#common-keywords
    // https://drafts.csswg.org/css-cascade-4/#valdef-all-revert
    bool is_css_wide_keyword() const { return is_inherit() || is_initial() || is_revert() || is_unset() || is_revert_layer(); }
    bool is_inherit() const { return to_keyword() == Keyword::Inherit; }
    bool is_initial() const { return to_keyword() == Keyword::Initial; }
    bool is_revert() const { return to_keyword() == Keyword::Revert; }
    bool is_revert_layer() const { return to_keyword() == Keyword::RevertLayer; }
    bool is_unset() const { return to_keyword() == Keyword::Unset; }

    bool has_auto() const;
    virtual bool has_color() const { return false; }

    virtual ValueComparingNonnullRefPtr<CSSStyleValue const> absolutized(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const;

    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const { return {}; }
    Keyword to_keyword() const;

    enum class SerializationMode {
        Normal,
        ResolvedValue,
    };
    virtual String to_string(SerializationMode) const = 0;

    [[nodiscard]] int to_font_weight() const;
    [[nodiscard]] int to_font_slope() const;
    [[nodiscard]] int to_font_width() const;

    virtual void visit_edges(JS::Cell::Visitor&) const { }

    virtual bool equals(CSSStyleValue const& other) const = 0;

    bool operator==(CSSStyleValue const& other) const
    {
        return this->equals(other);
    }

protected:
    explicit CSSStyleValue(Type);

private:
    Type m_type;
};

template<typename T>
struct StyleValueWithDefaultOperators : public CSSStyleValue {
    using CSSStyleValue::CSSStyleValue;

    virtual bool equals(CSSStyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& typed_other = static_cast<T const&>(other);
        return static_cast<T const&>(*this).properties_equal(typed_other);
    }
};

}

template<>
struct AK::Formatter<Web::CSS::CSSStyleValue> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::CSSStyleValue const& style_value)
    {
        return Formatter<StringView>::format(builder, style_value.to_string(Web::CSS::CSSStyleValue::SerializationMode::Normal));
    }
};
