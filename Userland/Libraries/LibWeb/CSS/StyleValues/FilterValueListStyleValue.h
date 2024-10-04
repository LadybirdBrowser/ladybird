/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Number.h>
#include <LibWeb/CSS/PercentageOr.h>

namespace Web::CSS {

namespace Filter {

struct Blur {
    Optional<Length> radius {};
    float resolved_radius(Layout::Node const&) const;
    bool operator==(Blur const&) const = default;
};

struct DropShadow {
    Length offset_x;
    Length offset_y;
    Optional<Length> radius {};
    Optional<Color> color {};
    bool operator==(DropShadow const&) const = default;
};

struct HueRotate {
    struct Zero {
        bool operator==(Zero const&) const = default;
    };
    using AngleOrZero = Variant<Angle, Zero>;
    Optional<AngleOrZero> angle {};
    float angle_degrees() const;
    bool operator==(HueRotate const&) const = default;
};

struct Color {
    enum class Operation {
        Brightness,
        Contrast,
        Grayscale,
        Invert,
        Opacity,
        Saturate,
        Sepia
    } operation;
    Optional<NumberPercentage> amount {};
    float resolved_amount() const;
    bool operator==(Color const&) const = default;
};

};

using FilterFunction = Variant<Filter::Blur, Filter::DropShadow, Filter::HueRotate, Filter::Color>;

class FilterValueListStyleValue final : public StyleValueWithDefaultOperators<FilterValueListStyleValue> {
public:
    static ValueComparingNonnullRefPtr<FilterValueListStyleValue> create(
        Vector<FilterFunction> filter_value_list)
    {
        VERIFY(filter_value_list.size() >= 1);
        return adopt_ref(*new (nothrow) FilterValueListStyleValue(move(filter_value_list)));
    }

    Vector<FilterFunction> const& filter_value_list() const { return m_filter_value_list; }

    virtual String to_string() const override;

    virtual ~FilterValueListStyleValue() override = default;

    bool properties_equal(FilterValueListStyleValue const& other) const { return m_filter_value_list == other.m_filter_value_list; }

private:
    FilterValueListStyleValue(Vector<FilterFunction> filter_value_list)
        : StyleValueWithDefaultOperators(Type::FilterValueList)
        , m_filter_value_list(move(filter_value_list))
    {
    }

    // FIXME: No support for SVG filters yet
    Vector<FilterFunction> m_filter_value_list;
};

}
