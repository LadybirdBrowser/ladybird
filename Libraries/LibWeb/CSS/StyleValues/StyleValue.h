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
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/ValueComparingRefPtr.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibGfx/Color.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/CalculationResolutionContext.h>
#include <LibWeb/CSS/Keyword.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

#define ENUMERATE_CSS_STYLE_VALUE_TYPES                                                                               \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(AddFunction, add_function, AddFunctionStyleValue)                                \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Anchor, anchor, AnchorStyleValue)                                                \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(AnchorSize, anchor_size, AnchorSizeStyleValue)                                   \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Angle, angle, AngleStyleValue)                                                   \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(BackgroundSize, background_size, BackgroundSizeStyleValue)                       \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(BasicShape, basic_shape, BasicShapeStyleValue)                                   \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(BorderImageSlice, border_image_slice, BorderImageSliceStyleValue)                \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(BorderRadius, border_radius, BorderRadiusStyleValue)                             \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(BorderRadiusRect, border_radius_rect, BorderRadiusRectStyleValue)                \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Calculated, calculated, CalculatedStyleValue)                                    \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(ColorScheme, color_scheme, ColorSchemeStyleValue)                                \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Color, color, ColorStyleValue)                                                   \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(ConicGradient, conic_gradient, ConicGradientStyleValue)                          \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Content, content, ContentStyleValue)                                             \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Counter, counter, CounterStyleValue)                                             \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(CounterStyle, counter_style, CounterStyleStyleValue)                             \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(CounterDefinitions, counter_definitions, CounterDefinitionsStyleValue)           \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(CounterStyleSystem, counter_style_system, CounterStyleSystemStyleValue)          \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Cursor, cursor, CursorStyleValue)                                                \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(CustomIdent, custom_ident, CustomIdentStyleValue)                                \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Display, display, DisplayStyleValue)                                             \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Easing, easing, EasingStyleValue)                                                \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Edge, edge, EdgeStyleValue)                                                      \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(FilterValueList, filter_value_list, FilterValueListStyleValue)                   \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(FitContent, fit_content, FitContentStyleValue)                                   \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Flex, flex, FlexStyleValue)                                                      \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(FontSource, font_source, FontSourceStyleValue)                                   \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(FontStyle, font_style, FontStyleStyleValue)                                      \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Frequency, frequency, FrequencyStyleValue)                                       \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(GridAutoFlow, grid_auto_flow, GridAutoFlowStyleValue)                            \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(GridTemplateArea, grid_template_area, GridTemplateAreaStyleValue)                \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(GridTrackPlacement, grid_track_placement, GridTrackPlacementStyleValue)          \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(GridTrackSizeList, grid_track_size_list, GridTrackSizeListStyleValue)            \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(GuaranteedInvalid, guaranteed_invalid, GuaranteedInvalidStyleValue)              \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Image, image, ImageStyleValue)                                                   \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Integer, integer, IntegerStyleValue)                                             \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Keyword, keyword, KeywordStyleValue)                                             \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Length, length, LengthStyleValue)                                                \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(LinearGradient, linear_gradient, LinearGradientStyleValue)                       \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Number, number, NumberStyleValue)                                                \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(OpenTypeTagged, open_type_tagged, OpenTypeTaggedStyleValue)                      \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(PendingSubstitution, pending_substitution, PendingSubstitutionStyleValue)        \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Percentage, percentage, PercentageStyleValue)                                    \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Position, position, PositionStyleValue)                                          \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(RadialGradient, radial_gradient, RadialGradientStyleValue)                       \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(RadialSize, radial_size, RadialSizeStyleValue)                                   \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(RandomValueSharing, random_value_sharing, RandomValueSharingStyleValue)          \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Ratio, ratio, RatioStyleValue)                                                   \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Rect, rect, RectStyleValue)                                                      \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(RepeatStyle, repeat_style, RepeatStyleStyleValue)                                \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Resolution, resolution, ResolutionStyleValue)                                    \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(ScrollbarColor, scrollbar_color, ScrollbarColorStyleValue)                       \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(ScrollbarGutter, scrollbar_gutter, ScrollbarGutterStyleValue)                    \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(ScrollFunction, scroll_function, ScrollFunctionStyleValue)                       \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Shadow, shadow, ShadowStyleValue)                                                \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Shorthand, shorthand, ShorthandStyleValue)                                       \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(String, string, StringStyleValue)                                                \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Superellipse, superellipse, SuperellipseStyleValue)                              \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(TextIndent, text_indent, TextIndentStyleValue)                                   \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(TextUnderlinePosition, text_underline_position, TextUnderlinePositionStyleValue) \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Time, time, TimeStyleValue)                                                      \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Transformation, transformation, TransformationStyleValue)                        \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(TreeCountingFunction, tree_counting_function, TreeCountingFunctionStyleValue)    \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(UnicodeRange, unicode_range, UnicodeRangeStyleValue)                             \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(Unresolved, unresolved, UnresolvedStyleValue)                                    \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(URL, url, URLStyleValue)                                                         \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(ValueList, value_list, StyleValueList)                                           \
    __ENUMERATE_CSS_STYLE_VALUE_TYPE(ViewFunction, view_function, ViewFunctionStyleValue)

struct ColorResolutionContext {
    Optional<PreferredColorScheme> color_scheme;
    Optional<Color> current_color;
    Optional<Color> accent_color;
    GC::Ptr<DOM::Document const> document;
    CalculationResolutionContext calculation_resolution_context;

    [[nodiscard]] static ColorResolutionContext for_element(DOM::AbstractElement const&);
    [[nodiscard]] static ColorResolutionContext for_layout_node_with_style(Layout::NodeWithStyle const&);
};

class WEB_API StyleValue : public RefCounted<StyleValue> {
public:
    virtual ~StyleValue() = default;

    enum class Type {
#define __ENUMERATE_CSS_STYLE_VALUE_TYPE(title_case, snake_case, style_value_class_name) title_case,
        ENUMERATE_CSS_STYLE_VALUE_TYPES
#undef __ENUMERATE_CSS_STYLE_VALUE_TYPE
    };

    Type type() const { return m_type; }

    bool is_abstract_image() const
    {
        return AK::first_is_one_of(type(), Type::Image, Type::LinearGradient, Type::ConicGradient, Type::RadialGradient);
    }
    AbstractImageStyleValue const& as_abstract_image() const;
    AbstractImageStyleValue& as_abstract_image() { return const_cast<AbstractImageStyleValue&>(const_cast<StyleValue const&>(*this).as_abstract_image()); }

    bool is_dimension() const
    {
        return first_is_one_of(type(), Type::Angle, Type::Flex, Type::Frequency, Type::Length, Type::Percentage, Type::Resolution, Type::Time);
    }
    DimensionStyleValue const& as_dimension() const;
    DimensionStyleValue& as_dimension() { return const_cast<DimensionStyleValue&>(const_cast<StyleValue const&>(*this).as_dimension()); }

    virtual bool is_color_function() const { return false; }

#define __ENUMERATE_CSS_STYLE_VALUE_TYPE(title_case, snake_case, style_value_class_name) \
    bool is_##snake_case() const { return type() == Type::title_case; }                  \
    style_value_class_name const& as_##snake_case() const;                               \
    style_value_class_name& as_##snake_case() { return const_cast<style_value_class_name&>(const_cast<StyleValue const&>(*this).as_##snake_case()); }
    ENUMERATE_CSS_STYLE_VALUE_TYPES
#undef __ENUMERATE_CSS_STYLE_VALUE_TYPE

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

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    virtual Optional<Color> to_color(ColorResolutionContext) const { return {}; }
    Keyword to_keyword() const;

    String to_string(SerializationMode) const;
    virtual void serialize(StringBuilder&, SerializationMode) const = 0;
    virtual Vector<Parser::ComponentValue> tokenize() const;
    virtual GC::Ref<CSSStyleValue> reify(JS::Realm&, FlyString const& associated_property) const;
    virtual StyleValueVector subdivide_into_iterations(PropertyNameAndID const&) const;

    virtual void set_style_sheet(GC::Ptr<CSSStyleSheet>) { }
    virtual void visit_edges(JS::Cell::Visitor&) const { }

    virtual bool equals(StyleValue const& other) const = 0;

    bool operator==(StyleValue const& other) const
    {
        return this->equals(other);
    }

protected:
    explicit StyleValue(Type);

private:
    Type m_type;
};

template<typename T>
struct StyleValueWithDefaultOperators : public StyleValue {
    using StyleValue::StyleValue;
    using Base = StyleValue;

    virtual bool equals(StyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& typed_other = static_cast<T const&>(other);
        return static_cast<T const&>(*this).properties_equal(typed_other);
    }
};

}

template<>
struct AK::Formatter<Web::CSS::StyleValue> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::StyleValue const& style_value)
    {
        return Formatter<StringView>::format(builder, style_value.to_string(Web::CSS::SerializationMode::Normal));
    }
};

template<>
struct AK::Formatter<Web::CSS::StyleValue::Type> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::StyleValue::Type type)
    {
        StringView type_name;
        switch (type) {
#define __ENUMERATE_CSS_STYLE_VALUE_TYPE(title_case, snake_case, style_value_class_name) \
    case Web::CSS::StyleValue::Type::title_case:                                         \
        type_name = #title_case##sv;                                                     \
        break;
            ENUMERATE_CSS_STYLE_VALUE_TYPES
#undef __ENUMERATE_CSS_STYLE_VALUE_TYPE
        default:
            VERIFY_NOT_REACHED();
        }

        return Formatter<StringView>::format(builder, type_name);
    }
};
