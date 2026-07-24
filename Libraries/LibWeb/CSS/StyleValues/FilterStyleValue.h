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
    void serialize(StringBuilder&, SerializationMode) const;
    bool equals(StyleValue const& other) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

protected:
    explicit FilterStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValue(Type::Filter, data)
        , m_filter_value(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data->filter.value.pointer))))
    {
    }

    FilterStyleValue(StyleValueFFI::StyleValueData const* data, ValueComparingNonnullRefPtr<StyleValue const> value)
        : StyleValue(Type::Filter, data)
        , m_filter_value(move(value))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> filter_value() const { return m_filter_value; }

    static StyleValueFFI::StyleValueData const* make_filter_data(Kind kind, u8 color_operation, StyleValue const* value)
    {
        return StyleValueFFI::rust_style_value_create_filter(to_underlying(kind), color_operation, StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()));
    }

private:
    ValueComparingNonnullRefPtr<StyleValue const> m_filter_value;
};

// https://drafts.csswg.org/filter-effects-1/#funcdef-filter-blur
class BlurFilterStyleValue final : public FilterStyleValue {
public:
    static ValueComparingNonnullRefPtr<BlurFilterStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> radius)
    {
        return adopt_ref(*new (nothrow) BlurFilterStyleValue(move(radius)));
    }

    ValueComparingNonnullRefPtr<StyleValue const> radius() const { return filter_value(); }
    float resolved_radius() const;

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool equals(StyleValue const&) const;

private:
    friend class StyleValue;

    explicit BlurFilterStyleValue(ValueComparingNonnullRefPtr<StyleValue const> radius)
        : FilterStyleValue(make_filter_data(Kind::Blur, 0, radius.ptr()), radius)
    {
    }

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
        return create(ShadowStyleValue::create(
            ShadowStyleValue::ShadowType::Text,
            move(color),
            move(offset_x),
            move(offset_y),
            move(radius),
            nullptr,
            ShadowPlacement::Outer));
    }

    static ValueComparingNonnullRefPtr<DropShadowFilterStyleValue const> create(ValueComparingNonnullRefPtr<ShadowStyleValue const> shadow)
    {
        return adopt_ref(*new (nothrow) DropShadowFilterStyleValue(move(shadow)));
    }

    ValueComparingNonnullRefPtr<ShadowStyleValue const> shadow_style_value() const { return shadow(); }
    ShadowStyleValue const& shadow() const { return filter_value()->as_shadow(); }
    ValueComparingNonnullRefPtr<StyleValue const> offset_x() const { return shadow().offset_x(); }
    ValueComparingNonnullRefPtr<StyleValue const> offset_y() const { return shadow().offset_y(); }
    ValueComparingRefPtr<StyleValue const> radius() const { return shadow().blur_radius_or_null(); }
    ValueComparingRefPtr<StyleValue const> color() const { return shadow().color_or_null(); }

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool equals(StyleValue const&) const;

private:
    friend class StyleValue;

    explicit DropShadowFilterStyleValue(ValueComparingNonnullRefPtr<ShadowStyleValue const> shadow)
        : FilterStyleValue(make_filter_data(Kind::DropShadow, 0, shadow.ptr()), shadow)
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
    static ValueComparingNonnullRefPtr<HueRotateFilterStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> angle)
    {
        return adopt_ref(*new (nothrow) HueRotateFilterStyleValue(move(angle)));
    }

    ValueComparingNonnullRefPtr<StyleValue const> angle() const { return filter_value(); }
    float angle_degrees() const;

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool equals(StyleValue const&) const;

private:
    friend class StyleValue;

    explicit HueRotateFilterStyleValue(ValueComparingNonnullRefPtr<StyleValue const> angle)
        : FilterStyleValue(make_filter_data(Kind::HueRotate, 0, angle.ptr()), angle)
    {
    }

    explicit HueRotateFilterStyleValue(StyleValueFFI::StyleValueData const* data)
        : FilterStyleValue(data)
    {
    }
};

// https://drafts.csswg.org/filter-effects-1/#supported-filter-functions
// <brightness()> | <contrast()> | <grayscale()> | <invert()> | <opacity()> | <sepia()> | <saturate()>
class ColorFilterStyleValue final : public FilterStyleValue {
public:
    static ValueComparingNonnullRefPtr<ColorFilterStyleValue const> create(Gfx::ColorFilterType operation, ValueComparingNonnullRefPtr<StyleValue const> amount)
    {
        return adopt_ref(*new (nothrow) ColorFilterStyleValue(operation, move(amount)));
    }

    Gfx::ColorFilterType operation() const { return static_cast<Gfx::ColorFilterType>(m_value->filter.color_operation); }
    ValueComparingNonnullRefPtr<StyleValue const> amount() const { return filter_value(); }
    float resolved_amount() const;

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool equals(StyleValue const&) const;

private:
    friend class StyleValue;

    ColorFilterStyleValue(Gfx::ColorFilterType operation, ValueComparingNonnullRefPtr<StyleValue const> amount)
        : FilterStyleValue(make_filter_data(Kind::Color, static_cast<u8>(to_underlying(operation)), amount.ptr()), amount)
    {
    }

    explicit ColorFilterStyleValue(StyleValueFFI::StyleValueData const* data)
        : FilterStyleValue(data)
    {
    }
};

bool is_filter_style_value_list(StyleValue const&);

}
