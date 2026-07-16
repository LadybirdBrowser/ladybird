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

    Kind kind() const { return m_kind; }
    virtual bool contains_url() const { return false; }
    static ValueComparingNonnullRefPtr<FilterStyleValue const> initial_value_for(FilterStyleValue const&, bool use_transparent_drop_shadow_color);

protected:
    explicit FilterStyleValue(Kind kind)
        : StyleValue(Type::Filter)
        , m_kind(kind)
    {
    }

private:
    Kind m_kind;
};

// https://drafts.csswg.org/filter-effects-1/#funcdef-filter-blur
class BlurFilterStyleValue final : public FilterStyleValue {
public:
    static ValueComparingNonnullRefPtr<BlurFilterStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> radius)
    {
        return adopt_ref(*new (nothrow) BlurFilterStyleValue(move(radius)));
    }

    ValueComparingNonnullRefPtr<StyleValue const> const& radius() const { return m_radius; }
    float resolved_radius() const;

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual bool is_computationally_independent() const override { return m_radius->is_computationally_independent(); }
    virtual bool equals(StyleValue const&) const override;

private:
    explicit BlurFilterStyleValue(ValueComparingNonnullRefPtr<StyleValue const> radius)
        : FilterStyleValue(Kind::Blur)
        , m_radius(move(radius))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> m_radius;
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

    ValueComparingNonnullRefPtr<ShadowStyleValue const> const& shadow_style_value() const { return m_shadow; }
    ShadowStyleValue const& shadow() const { return m_shadow; }
    ValueComparingNonnullRefPtr<StyleValue const> offset_x() const { return m_shadow->offset_x(); }
    ValueComparingNonnullRefPtr<StyleValue const> offset_y() const { return m_shadow->offset_y(); }
    ValueComparingRefPtr<StyleValue const> const& radius() const { return m_shadow->blur_radius_or_null(); }
    ValueComparingRefPtr<StyleValue const> const& color() const { return m_shadow->color_or_null(); }

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual bool is_computationally_independent() const override
    {
        return m_shadow->is_computationally_independent();
    }
    virtual bool equals(StyleValue const&) const override;

private:
    explicit DropShadowFilterStyleValue(ValueComparingNonnullRefPtr<ShadowStyleValue const> shadow)
        : FilterStyleValue(Kind::DropShadow)
        , m_shadow(move(shadow))
    {
    }

    ValueComparingNonnullRefPtr<ShadowStyleValue const> m_shadow;
};

// https://drafts.csswg.org/filter-effects-1/#funcdef-filter-hue-rotate
class HueRotateFilterStyleValue final : public FilterStyleValue {
public:
    static ValueComparingNonnullRefPtr<HueRotateFilterStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> angle)
    {
        return adopt_ref(*new (nothrow) HueRotateFilterStyleValue(move(angle)));
    }

    ValueComparingNonnullRefPtr<StyleValue const> const& angle() const { return m_angle; }
    float angle_degrees() const;

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual bool is_computationally_independent() const override { return m_angle->is_computationally_independent(); }
    virtual bool equals(StyleValue const&) const override;

private:
    explicit HueRotateFilterStyleValue(ValueComparingNonnullRefPtr<StyleValue const> angle)
        : FilterStyleValue(Kind::HueRotate)
        , m_angle(move(angle))
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> m_angle;
};

// https://drafts.csswg.org/filter-effects-1/#supported-filter-functions
// <brightness()> | <contrast()> | <grayscale()> | <invert()> | <opacity()> | <sepia()> | <saturate()>
class ColorFilterStyleValue final : public FilterStyleValue {
public:
    static ValueComparingNonnullRefPtr<ColorFilterStyleValue const> create(Gfx::ColorFilterType operation, ValueComparingNonnullRefPtr<StyleValue const> amount)
    {
        return adopt_ref(*new (nothrow) ColorFilterStyleValue(operation, move(amount)));
    }

    Gfx::ColorFilterType operation() const { return m_operation; }
    ValueComparingNonnullRefPtr<StyleValue const> const& amount() const { return m_amount; }
    float resolved_amount() const;

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual bool is_computationally_independent() const override { return m_amount->is_computationally_independent(); }
    virtual bool equals(StyleValue const&) const override;

private:
    ColorFilterStyleValue(Gfx::ColorFilterType operation, ValueComparingNonnullRefPtr<StyleValue const> amount)
        : FilterStyleValue(Kind::Color)
        , m_operation(operation)
        , m_amount(move(amount))
    {
    }

    Gfx::ColorFilterType m_operation;
    ValueComparingNonnullRefPtr<StyleValue const> m_amount;
};

bool is_filter_style_value_list(StyleValue const&);

}
