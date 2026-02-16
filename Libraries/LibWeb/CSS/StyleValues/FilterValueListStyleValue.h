/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Filter.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/URL.h>

namespace Web::CSS {

namespace FilterOperation {

struct Blur {
    ValueComparingNonnullRefPtr<StyleValue const> radius;
    float resolved_radius() const;
    bool operator==(Blur const&) const = default;
};

// FIXME: It would be nice if we could use a ShadowStyleValue here
struct DropShadow {
    ValueComparingNonnullRefPtr<StyleValue const> offset_x;
    ValueComparingNonnullRefPtr<StyleValue const> offset_y;
    ValueComparingRefPtr<StyleValue const> radius;
    ValueComparingRefPtr<StyleValue const> color;
    bool operator==(DropShadow const&) const = default;
};

struct HueRotate {
    ValueComparingNonnullRefPtr<StyleValue const> angle;
    float angle_degrees() const;
    bool operator==(HueRotate const&) const = default;
};

struct Color {
    Gfx::ColorFilterType operation;
    ValueComparingNonnullRefPtr<StyleValue const> amount;
    float resolved_amount() const;
    bool operator==(Color const&) const = default;
};

};

class FilterValueListStyleValue final : public StyleValueWithDefaultOperators<FilterValueListStyleValue> {
public:
    static ValueComparingNonnullRefPtr<FilterValueListStyleValue const> create(
        Vector<FilterValue> filter_value_list)
    {
        VERIFY(filter_value_list.size() >= 1);
        return adopt_ref(*new (nothrow) FilterValueListStyleValue(move(filter_value_list)));
    }

    Vector<FilterValue> const& filter_value_list() const { return m_filter_value_list; }
    size_t size() const { return m_filter_value_list.size(); }

    bool contains_url() const;

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    virtual ~FilterValueListStyleValue() override = default;

    bool properties_equal(FilterValueListStyleValue const& other) const { return m_filter_value_list == other.m_filter_value_list; }

private:
    FilterValueListStyleValue(Vector<FilterValue> filter_value_list)
        : StyleValueWithDefaultOperators(Type::FilterValueList)
        , m_filter_value_list(move(filter_value_list))
    {
    }

    // FIXME: No support for SVG filters yet
    Vector<FilterValue> m_filter_value_list;
};

}
