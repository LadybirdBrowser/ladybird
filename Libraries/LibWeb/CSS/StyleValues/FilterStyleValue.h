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
    virtual bool contains_url() const { return false; }
    static ValueComparingNonnullRefPtr<FilterStyleValue const> initial_value_for(FilterStyleValue const&, bool use_transparent_drop_shadow_color);

protected:
    explicit FilterStyleValue(StyleValueFFI::StyleValueData* data)
        : StyleValue(Type::Filter, data)
    {
    }

    static StyleValueFFI::StyleValueData* make_filter_data(Kind kind, u8 color_operation, StyleValue const* value)
    {
        // The Rust allocation takes ownership of one strong reference to the value.
        return StyleValueFFI::rust_style_value_create_filter(to_underlying(kind), color_operation, retain_style_value_for_rust(value));
    }
};

// https://drafts.csswg.org/filter-effects-1/#funcdef-filter-blur
class BlurFilterStyleValue final : public FilterStyleValue {
public:
    static ValueComparingNonnullRefPtr<BlurFilterStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> radius)
    {
        return adopt_ref(*new (nothrow) BlurFilterStyleValue(move(radius)));
    }

    ValueComparingNonnullRefPtr<StyleValue const> radius() const { return *static_cast<StyleValue const*>(m_value->filter.value.pointer); }
    float resolved_radius() const;

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool equals(StyleValue const&) const;

private:
    explicit BlurFilterStyleValue(ValueComparingNonnullRefPtr<StyleValue const> radius)
        : FilterStyleValue(make_filter_data(Kind::Blur, 0, radius.ptr()))
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
    ShadowStyleValue const& shadow() const { return *static_cast<ShadowStyleValue const*>(m_value->filter.value.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> offset_x() const { return shadow().offset_x(); }
    ValueComparingNonnullRefPtr<StyleValue const> offset_y() const { return shadow().offset_y(); }
    ValueComparingRefPtr<StyleValue const> radius() const { return shadow().blur_radius_or_null(); }
    ValueComparingRefPtr<StyleValue const> color() const { return shadow().color_or_null(); }

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool equals(StyleValue const&) const;

private:
    explicit DropShadowFilterStyleValue(ValueComparingNonnullRefPtr<ShadowStyleValue const> shadow)
        : FilterStyleValue(make_filter_data(Kind::DropShadow, 0, shadow.ptr()))
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

    ValueComparingNonnullRefPtr<StyleValue const> angle() const { return *static_cast<StyleValue const*>(m_value->filter.value.pointer); }
    float angle_degrees() const;

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool equals(StyleValue const&) const;

private:
    explicit HueRotateFilterStyleValue(ValueComparingNonnullRefPtr<StyleValue const> angle)
        : FilterStyleValue(make_filter_data(Kind::HueRotate, 0, angle.ptr()))
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
    ValueComparingNonnullRefPtr<StyleValue const> amount() const { return *static_cast<StyleValue const*>(m_value->filter.value.pointer); }
    float resolved_amount() const;

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool equals(StyleValue const&) const;

private:
    ColorFilterStyleValue(Gfx::ColorFilterType operation, ValueComparingNonnullRefPtr<StyleValue const> amount)
        : FilterStyleValue(make_filter_data(Kind::Color, static_cast<u8>(to_underlying(operation)), amount.ptr()))
    {
    }
};

bool is_filter_style_value_list(StyleValue const&);

}
