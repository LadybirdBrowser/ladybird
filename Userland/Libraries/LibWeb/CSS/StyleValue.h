/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/GenericShorthands.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibGfx/Color.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/ValueID.h>
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

using StyleValueVector = Vector<ValueComparingNonnullRefPtr<StyleValue const>>;

#define ENUMERATE_STYLE_VALUE_TYPES                                        \
    __ENUMERATE_STYLE_VALUE_TYPE(Angle, angle)                             \
    __ENUMERATE_STYLE_VALUE_TYPE(BackgroundRepeat, background_repeat)      \
    __ENUMERATE_STYLE_VALUE_TYPE(BackgroundSize, background_size)          \
    __ENUMERATE_STYLE_VALUE_TYPE(BasicShape, basic_shape)                  \
    __ENUMERATE_STYLE_VALUE_TYPE(BorderRadius, border_radius)              \
    __ENUMERATE_STYLE_VALUE_TYPE(Calculated, calculated)                   \
    __ENUMERATE_STYLE_VALUE_TYPE(Color, color)                             \
    __ENUMERATE_STYLE_VALUE_TYPE(ConicGradient, conic_gradient)            \
    __ENUMERATE_STYLE_VALUE_TYPE(Content, content)                         \
    __ENUMERATE_STYLE_VALUE_TYPE(CustomIdent, custom_ident)                \
    __ENUMERATE_STYLE_VALUE_TYPE(Display, display)                         \
    __ENUMERATE_STYLE_VALUE_TYPE(Easing, easing)                           \
    __ENUMERATE_STYLE_VALUE_TYPE(Edge, edge)                               \
    __ENUMERATE_STYLE_VALUE_TYPE(FilterValueList, filter_value_list)       \
    __ENUMERATE_STYLE_VALUE_TYPE(Flex, flex)                               \
    __ENUMERATE_STYLE_VALUE_TYPE(Frequency, frequency)                     \
    __ENUMERATE_STYLE_VALUE_TYPE(GridAutoFlow, grid_auto_flow)             \
    __ENUMERATE_STYLE_VALUE_TYPE(GridTemplateArea, grid_template_area)     \
    __ENUMERATE_STYLE_VALUE_TYPE(GridTrackPlacement, grid_track_placement) \
    __ENUMERATE_STYLE_VALUE_TYPE(GridTrackSizeList, grid_track_size_list)  \
    __ENUMERATE_STYLE_VALUE_TYPE(Identifier, identifier)                   \
    __ENUMERATE_STYLE_VALUE_TYPE(Image, image)                             \
    __ENUMERATE_STYLE_VALUE_TYPE(Inherit, inherit)                         \
    __ENUMERATE_STYLE_VALUE_TYPE(Initial, initial)                         \
    __ENUMERATE_STYLE_VALUE_TYPE(Integer, integer)                         \
    __ENUMERATE_STYLE_VALUE_TYPE(Length, length)                           \
    __ENUMERATE_STYLE_VALUE_TYPE(LinearGradient, linear_gradient)          \
    __ENUMERATE_STYLE_VALUE_TYPE(MathDepth, math_depth)                    \
    __ENUMERATE_STYLE_VALUE_TYPE(Number, number)                           \
    __ENUMERATE_STYLE_VALUE_TYPE(Percentage, percentage)                   \
    __ENUMERATE_STYLE_VALUE_TYPE(Position, position)                       \
    __ENUMERATE_STYLE_VALUE_TYPE(RadialGradient, radial_gradient)          \
    __ENUMERATE_STYLE_VALUE_TYPE(Ratio, ratio)                             \
    __ENUMERATE_STYLE_VALUE_TYPE(Rect, rect)                               \
    __ENUMERATE_STYLE_VALUE_TYPE(Resolution, resolution)                   \
    __ENUMERATE_STYLE_VALUE_TYPE(Revert, revert)                           \
    __ENUMERATE_STYLE_VALUE_TYPE(ScrollbarGutter, scrollbar_gutter)        \
    __ENUMERATE_STYLE_VALUE_TYPE(Shadow, shadow)                           \
    __ENUMERATE_STYLE_VALUE_TYPE(Shorthand, shorthand)                     \
    __ENUMERATE_STYLE_VALUE_TYPE(String, string)                           \
    __ENUMERATE_STYLE_VALUE_TYPE(Time, time)                               \
    __ENUMERATE_STYLE_VALUE_TYPE(Transformation, transformation)           \
    __ENUMERATE_STYLE_VALUE_TYPE(Transition, transition)                   \
    __ENUMERATE_STYLE_VALUE_TYPE(Unresolved, unresolved)                   \
    __ENUMERATE_STYLE_VALUE_TYPE(Unset, unset)                             \
    __ENUMERATE_STYLE_VALUE_TYPE(URL, url)                                 \
    __ENUMERATE_STYLE_VALUE_TYPE(ValueList, value_list)

// NOTE:
using ValueListStyleValue = StyleValueList;

class StyleValue : public RefCounted<StyleValue> {
public:
    virtual ~StyleValue() = default;

    enum class Type {
#define __ENUMERATE_STYLE_VALUE_TYPE(TitleCaseName, SnakeCaseName) \
    TitleCaseName,
        ENUMERATE_STYLE_VALUE_TYPES
#undef __ENUMERATE_STYLE_VALUE_TYPE
    };

    Type type() const { return m_type; }

#define __ENUMERATE_STYLE_VALUE_TYPE(TitleCaseName, SnakeCaseName)            \
    bool is_##SnakeCaseName() const { return type() == Type::TitleCaseName; } \
    TitleCaseName##StyleValue const& as_##SnakeCaseName() const;              \
    TitleCaseName##StyleValue& as_##SnakeCaseName() { return const_cast<TitleCaseName##StyleValue&>(const_cast<StyleValue const&>(*this).as_##SnakeCaseName()); }
    ENUMERATE_STYLE_VALUE_TYPES
#undef __ENUMERATE_STYLE_VALUE_TYPE

    bool is_abstract_image() const
    {
        return AK::first_is_one_of(type(), Type::Image, Type::LinearGradient, Type::ConicGradient, Type::RadialGradient);
    }
    AbstractImageStyleValue const& as_abstract_image() const;
    AbstractImageStyleValue& as_abstract_image() { return const_cast<AbstractImageStyleValue&>(const_cast<StyleValue const&>(*this).as_abstract_image()); }

    // https://www.w3.org/TR/css-values-4/#common-keywords
    // https://drafts.csswg.org/css-cascade-4/#valdef-all-revert
    bool is_css_wide_keyword() const { return is_inherit() || is_initial() || is_revert() || is_unset(); }

    bool has_auto() const;
    virtual bool has_color() const { return false; }

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const;

    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const { return {}; }
    ValueID to_identifier() const;
    virtual String to_string() const = 0;

    [[nodiscard]] int to_font_weight() const;
    [[nodiscard]] int to_font_slope() const;
    [[nodiscard]] int to_font_stretch_width() const;

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
        return Formatter<StringView>::format(builder, style_value.to_string());
    }
};
