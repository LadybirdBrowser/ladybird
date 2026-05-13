/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Variant.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Number.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

// FIXME: This should probably instead be CSSPixelsPercentage since it is only used after computation where we resolve
//        all relative lengths.

class LengthPercentage {
public:
    LengthPercentage(Length t)
        : m_value(move(t))
    {
    }

    LengthPercentage(Percentage percentage)
        : m_value(move(percentage))
    {
    }

    LengthPercentage(NonnullRefPtr<CalculatedStyleValue const> calculated)
        : m_value(move(calculated))
    {
    }

    ~LengthPercentage() = default;

    bool contains_percentage() const
    {
        return m_value.visit(
            [&](Length const&) {
                return false;
            },
            [&](Percentage const&) {
                return true;
            },
            [&](NonnullRefPtr<CalculatedStyleValue const> const& calculated) {
                return calculated->contains_percentage();
            });
    }

    Percentage const& percentage() const
    {
        VERIFY(is_percentage());
        return m_value.template get<Percentage>();
    }

    NonnullRefPtr<CalculatedStyleValue const> const& calculated() const
    {
        VERIFY(is_calculated());
        return m_value.template get<NonnullRefPtr<CalculatedStyleValue const>>();
    }

    CSSPixels to_px(CSSPixels reference_value) const
    {
        return resolved(reference_value).absolute_length_to_px();
    }

    Length resolved(CSSPixels reference_value) const
    {
        return m_value.visit(
            [&](Length const& t) {
                return t;
            },
            [&](Percentage const& percentage) {
                return Length::make_px(CSSPixels::truncated_value_for(reference_value.to_double() * percentage.as_fraction()));
            },
            [&](NonnullRefPtr<CalculatedStyleValue const> const& calculated) {
                return calculated->resolve_length({ .percentage_basis = Length::make_px(reference_value) }).value();
            });
    }

    void serialize(StringBuilder& builder, SerializationMode mode) const
    {
        if (is_calculated()) {
            m_value.template get<NonnullRefPtr<CalculatedStyleValue const>>()->serialize(builder, mode);
        } else if (is_percentage()) {
            m_value.template get<Percentage>().serialize(builder, mode);
        } else {
            m_value.template get<Length>().serialize(builder, mode);
        }
    }

    String to_string(SerializationMode mode) const
    {
        StringBuilder builder;
        serialize(builder, mode);
        return builder.to_string_without_validation();
    }

    static LengthPercentage from_style_value(NonnullRefPtr<StyleValue const> const& style_value)
    {
        if (style_value->is_percentage())
            return LengthPercentage { style_value->as_percentage().percentage() };
        if (style_value->is_length())
            return LengthPercentage { style_value->as_length().length() };
        if (style_value->is_calculated())
            return LengthPercentage { style_value->as_calculated() };

        VERIFY_NOT_REACHED();
    }

    bool is_percentage() const { return m_value.template has<Percentage>(); }
    bool is_calculated() const { return m_value.template has<NonnullRefPtr<CalculatedStyleValue const>>(); }
    bool is_length() const { return m_value.has<Length>(); }
    Length const& length() const { return m_value.get<Length>(); }

    bool operator==(LengthPercentage const& other) const = default;

private:
    Variant<Length, Percentage, NonnullRefPtr<CalculatedStyleValue const>> m_value;
};

class LengthPercentageOrAuto {
public:
    LengthPercentageOrAuto(LengthPercentage length_percentage)
        : m_length_percentage(move(length_percentage))
    {
    }

    LengthPercentageOrAuto(Length length)
        : m_length_percentage(move(length))
    {
    }

    LengthPercentageOrAuto(Percentage percentage)
        : m_length_percentage(move(percentage))
    {
    }

    static LengthPercentageOrAuto make_auto()
    {
        return LengthPercentageOrAuto();
    }

    static LengthPercentageOrAuto from_style_value(NonnullRefPtr<StyleValue const> const& style_value)
    {
        if (style_value->has_auto())
            return LengthPercentageOrAuto::make_auto();

        return LengthPercentage::from_style_value(style_value);
    }

    bool is_auto() const { return !m_length_percentage.has_value(); }
    bool is_length() const { return m_length_percentage.has_value() && m_length_percentage->is_length(); }
    bool is_percentage() const { return m_length_percentage.has_value() && m_length_percentage->is_percentage(); }
    bool is_calculated() const { return m_length_percentage.has_value() && m_length_percentage->is_calculated(); }

    bool contains_percentage() const { return m_length_percentage.has_value() && m_length_percentage->contains_percentage(); }

    LengthPercentage const& length_percentage() const { return m_length_percentage.value(); }
    Length const& length() const { return m_length_percentage->length(); }
    Percentage const& percentage() const { return m_length_percentage->percentage(); }
    NonnullRefPtr<CalculatedStyleValue const> const& calculated() const { return m_length_percentage->calculated(); }

    LengthOrAuto resolved_or_auto(CSSPixels reference_value) const
    {
        if (is_auto())
            return LengthOrAuto::make_auto();
        return length_percentage().resolved(reference_value);
    }

    CSSPixels to_px_or_zero(CSSPixels reference_value) const
    {
        if (is_auto())
            return 0;
        return length_percentage().to_px(reference_value);
    }

    void serialize(StringBuilder& builder, SerializationMode mode) const
    {
        if (is_auto())
            builder.append("auto"sv);
        else
            m_length_percentage->serialize(builder, mode);
    }

    String to_string(SerializationMode mode) const
    {
        StringBuilder builder;
        serialize(builder, mode);
        return builder.to_string_without_validation();
    }

    bool operator==(LengthPercentageOrAuto const&) const = default;

private:
    LengthPercentageOrAuto() = default;

    Optional<LengthPercentage> m_length_percentage;
};

}

template<>
struct AK::Formatter<Web::CSS::LengthPercentage> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::LengthPercentage const& length_percentage)
    {
        return Formatter<StringView>::format(builder, length_percentage.to_string(Web::CSS::SerializationMode::Normal));
    }
};

template<>
struct AK::Formatter<Web::CSS::LengthPercentageOrAuto> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::LengthPercentageOrAuto const& length_percentage_or_auto)
    {
        return Formatter<StringView>::format(builder, length_percentage_or_auto.to_string(Web::CSS::SerializationMode::Normal));
    }
};
