/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Filter.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class FilterStyleValue : public StyleValue {
public:
    enum class Kind : u8 {
        Blur,
        DropShadow,
        HueRotate,
        Color,
    };

    virtual ~FilterStyleValue() override = default;

    Kind kind() const { return static_cast<Kind>(m_value->filter.kind); }

protected:
    explicit FilterStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValue(Type::Filter, data)
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> filter_value() const { return wrap_rust_child(m_value->filter.value); }

    static StyleValueFFI::StyleValueData const* make_filter_data(Kind kind, u8 color_operation, StyleValue const* value)
    {
        return StyleValueFFI::rust_style_value_create_filter(to_underlying(kind), color_operation, StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()));
    }
};

// https://drafts.csswg.org/filter-effects-1/#funcdef-filter-blur
class BlurFilterStyleValue final : public FilterStyleValue {
public:
    ValueComparingNonnullRefPtr<StyleValue const> radius() const { return filter_value(); }
    float resolved_radius() const;

private:
    friend class StyleValue;

    explicit BlurFilterStyleValue(StyleValueFFI::StyleValueData const* data)
        : FilterStyleValue(data)
    {
    }
};

// https://drafts.csswg.org/filter-effects-1/#funcdef-filter-drop-shadow
class DropShadowFilterStyleValue final : public FilterStyleValue {
public:
    static ValueComparingNonnullRefPtr<DropShadowFilterStyleValue const> create(
        ValueComparingNonnullRefPtr<StyleValue const> offset_x,
        ValueComparingNonnullRefPtr<StyleValue const> offset_y,
        ValueComparingRefPtr<StyleValue const> radius,
        ValueComparingRefPtr<StyleValue const> color)
    {
        return adopt_ref(*new (nothrow) DropShadowFilterStyleValue(ShadowStyleValue::create(
            ShadowStyleValue::ShadowType::Text,
            move(color),
            move(offset_x),
            move(offset_y),
            move(radius),
            nullptr,
            ShadowPlacement::Outer)));
    }

    ValueComparingNonnullRefPtr<ShadowStyleValue const> shadow() const { return filter_value()->as_shadow(); }
    ValueComparingNonnullRefPtr<StyleValue const> offset_x() const { return shadow()->offset_x(); }
    ValueComparingNonnullRefPtr<StyleValue const> offset_y() const { return shadow()->offset_y(); }
    ValueComparingRefPtr<StyleValue const> radius() const { return shadow()->blur_radius_or_null(); }
    ValueComparingRefPtr<StyleValue const> color() const { return shadow()->color_or_null(); }

private:
    friend class StyleValue;

    explicit DropShadowFilterStyleValue(ValueComparingNonnullRefPtr<ShadowStyleValue const> shadow)
        : FilterStyleValue(make_filter_data(Kind::DropShadow, 0, shadow.ptr()))
    {
    }

    explicit DropShadowFilterStyleValue(StyleValueFFI::StyleValueData const* data)
        : FilterStyleValue(data)
    {
    }
};

// https://drafts.csswg.org/filter-effects-1/#funcdef-filter-hue-rotate
class HueRotateFilterStyleValue final : public FilterStyleValue {
public:
    ValueComparingNonnullRefPtr<StyleValue const> angle() const { return filter_value(); }
    float angle_degrees() const;

private:
    friend class StyleValue;

    explicit HueRotateFilterStyleValue(StyleValueFFI::StyleValueData const* data)
        : FilterStyleValue(data)
    {
    }
};

// https://drafts.csswg.org/filter-effects-1/#supported-filter-functions
// <brightness()> | <contrast()> | <grayscale()> | <invert()> | <opacity()> | <sepia()> | <saturate()>
class ColorFilterStyleValue final : public FilterStyleValue {
public:
    Gfx::ColorFilterType operation() const { return static_cast<Gfx::ColorFilterType>(m_value->filter.color_operation); }
    ValueComparingNonnullRefPtr<StyleValue const> amount() const { return filter_value(); }
    float resolved_amount() const;

private:
    friend class StyleValue;

    explicit ColorFilterStyleValue(StyleValueFFI::StyleValueData const* data)
        : FilterStyleValue(data)
    {
    }
};

bool is_filter_style_value_list(StyleValue const&);

}
