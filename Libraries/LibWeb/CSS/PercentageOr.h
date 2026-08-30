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
        : m_value(StyleValueFFI::rust_style_value_create_length(t.raw_value(), to_underlying(t.unit())))
    {
    }

    LengthPercentage(Percentage percentage)
        : m_value(StyleValueFFI::rust_style_value_create_percentage(percentage.value()))
    {
    }

    LengthPercentage(NonnullRefPtr<CalculatedStyleValue const> calculated)
        : m_value(StyleValueFFI::rust_style_value_retain(calculated->rust_style_value_data()))
    {
    }

    bool contains_percentage() const
    {
        if (is_percentage())
            return true;
        if (is_calculated())
            return calculated()->contains_percentage();
        return false;
    }

    Percentage percentage() const
    {
        VERIFY(is_percentage());
        return Percentage(m_value->percentage.value);
    }

    ValueComparingNonnullRefPtr<CalculatedStyleValue const> calculated() const
    {
        VERIFY(is_calculated());
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(m_value.data()))->as_calculated();
    }

    CSSPixels to_px(CSSPixels reference_value) const
    {
        if (!is_calculated())
            return resolved(reference_value).absolute_length_to_px();
        auto resolved_length = calculated()->resolve_length({ .percentage_basis = Length::make_px(reference_value) }).value();
        return CSSPixels::truncated_value_for(resolved_length.absolute_length_to_px_without_rounding());
    }

    Length resolved(CSSPixels reference_value) const
    {
        if (is_length())
            return length();
        if (is_percentage())
            return Length::make_px(CSSPixels::truncated_value_for(reference_value.to_double() * percentage().as_fraction()));
        return calculated()->resolve_length({ .percentage_basis = Length::make_px(reference_value) }).value();
    }

    void serialize(StringBuilder& builder, SerializationMode mode) const
    {
        if (is_calculated()) {
            calculated()->serialize(builder, mode);
        } else if (is_percentage()) {
            percentage().serialize(builder, mode);
        } else {
            length().serialize(builder, mode);
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
        VERIFY(style_value->is_percentage() || style_value->is_length() || (style_value->is_calculated() && style_value->as_calculated().resolves_to_length()));
        return from_retained_data(StyleValueFFI::rust_style_value_retain(style_value->rust_style_value_data()));
    }

    bool is_percentage() const { return m_value->tag == StyleValueFFI::StyleValueData::Tag::Percentage; }
    bool is_calculated() const { return m_value->tag == StyleValueFFI::StyleValueData::Tag::Calculated; }
    bool is_length() const { return m_value->tag == StyleValueFFI::StyleValueData::Tag::Length; }
    Length length() const
    {
        VERIFY(is_length());
        return Length(m_value->length.value, static_cast<LengthUnit>(m_value->length.unit));
    }

    bool operator==(LengthPercentage const& other) const
    {
        if (m_value.data() == other.m_value.data())
            return true;
        if (is_length() && other.is_length())
            return length() == other.length();
        if (is_percentage() && other.is_percentage())
            return percentage() == other.percentage();
        if (is_calculated() && other.is_calculated())
            return calculated()->equals(*other.calculated());
        return false;
    }

    template<typename Handle>
    static LengthPercentage const& view(Handle const& value)
    {
        static_assert(sizeof(LengthPercentage) == sizeof(value));
        static_assert(requires { value.pointer; });
        return reinterpret_cast<LengthPercentage const&>(value);
    }

    StyleValueFFI::StyleValueData const* leak_data() { return m_value.leak_data(); }

private:
    explicit LengthPercentage(StyleValueFFI::StyleValueData const* data)
        : m_value(data)
    {
    }

    static LengthPercentage from_retained_data(StyleValueFFI::StyleValueData const* data)
    {
        return LengthPercentage(data);
    }

    RustStyleValueHandle m_value;
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
    Length length() const { return m_length_percentage->length(); }
    Percentage percentage() const { return m_length_percentage->percentage(); }
    ValueComparingNonnullRefPtr<CalculatedStyleValue const> calculated() const { return m_length_percentage->calculated(); }

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
