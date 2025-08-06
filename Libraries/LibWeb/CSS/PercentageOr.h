/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Variant.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Number.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

template<typename T, typename Self>
class PercentageOr {
public:
    PercentageOr(T t)
        : m_value(move(t))
    {
    }

    PercentageOr(Percentage percentage)
        : m_value(move(percentage))
    {
    }

    PercentageOr(NonnullRefPtr<CalculatedStyleValue const> calculated)
        : m_value(move(calculated))
    {
    }

    ~PercentageOr() = default;

    PercentageOr<T, Self>& operator=(T t)
    {
        m_value = move(t);
        return *this;
    }

    PercentageOr<T, Self>& operator=(Percentage percentage)
    {
        m_value = move(percentage);
        return *this;
    }

    bool is_percentage() const { return m_value.template has<Percentage>(); }
    bool is_calculated() const { return m_value.template has<NonnullRefPtr<CalculatedStyleValue const>>(); }

    bool contains_percentage() const
    {
        return m_value.visit(
            [&](T const&) {
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

    CSSPixels to_px(Layout::Node const& layout_node, CSSPixels reference_value) const
    {
        if constexpr (IsSame<T, Length>) {
            if (auto const* length = m_value.template get_pointer<Length>()) {
                if (length->is_absolute())
                    return length->absolute_length_to_px();
            }
        }
        return resolved(layout_node, reference_value).to_px(layout_node);
    }

    T resolved(Layout::Node const& layout_node, T reference_value) const
    requires(!IsSame<T, Length>)
    {
        return m_value.visit(
            [&](T const& t) {
                return t;
            },
            [&](Percentage const& percentage) {
                return reference_value.percentage_of(percentage);
            },
            [&](NonnullRefPtr<CalculatedStyleValue const> const& calculated) {
                return T::resolve_calculated(calculated, layout_node, reference_value);
            });
    }

    T resolved(Layout::Node const& layout_node, CSSPixels reference_value) const
    {
        return m_value.visit(
            [&](T const& t) {
                return t;
            },
            [&](Percentage const& percentage) {
                return Length::make_px(CSSPixels(percentage.value() * reference_value) / 100);
            },
            [&](NonnullRefPtr<CalculatedStyleValue const> const& calculated) {
                return T::resolve_calculated(calculated, layout_node, reference_value);
            });
    }

    String to_string(SerializationMode mode) const
    {
        if (is_calculated())
            return m_value.template get<NonnullRefPtr<CalculatedStyleValue const>>()->to_string(mode);
        if (is_percentage())
            return m_value.template get<Percentage>().to_string();
        return m_value.template get<T>().to_string();
    }

    bool operator==(PercentageOr<T, Self> const& other) const
    {
        if (is_calculated() != other.is_calculated())
            return false;
        if (is_percentage() != other.is_percentage())
            return false;
        if (is_calculated())
            return (*m_value.template get<NonnullRefPtr<CalculatedStyleValue const>>() == *other.m_value.template get<NonnullRefPtr<CalculatedStyleValue const>>());
        if (is_percentage())
            return (m_value.template get<Percentage>() == other.m_value.template get<Percentage>());
        return (m_value.template get<T>() == other.m_value.template get<T>());
    }

    Self absolutized(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const
    {
        return m_value.visit(
            [&](T const& value) {
                if constexpr (IsSame<T, Length>)
                    return Self { value.absolutized(viewport_rect, font_metrics, root_font_metrics) };
                else
                    return *static_cast<Self const*>(this);
            },
            [&](Percentage const&) { return *static_cast<Self const*>(this); },
            [&](NonnullRefPtr<CalculatedStyleValue const> const& value) {
                return Self { value->absolutized(viewport_rect, font_metrics, root_font_metrics)->as_calculated() };
            });
    }

protected:
    bool is_t() const { return m_value.template has<T>(); }
    T const& get_t() const { return m_value.template get<T>(); }

private:
    Variant<T, Percentage, NonnullRefPtr<CalculatedStyleValue const>> m_value;
};

template<typename T, typename Self>
bool operator==(PercentageOr<T, Self> const& percentage_or, T const& t)
{
    return percentage_or == PercentageOr<T, Self> { t };
}

template<typename T, typename Self>
bool operator==(T const& t, PercentageOr<T, Self> const& percentage_or)
{
    return t == percentage_or;
}

template<typename T, typename Self>
bool operator==(PercentageOr<T, Self> const& percentage_or, Percentage const& percentage)
{
    return percentage_or == PercentageOr<T, Self> { percentage };
}

template<typename T, typename Self>
bool operator==(Percentage const& percentage, PercentageOr<T, Self> const& percentage_or)
{
    return percentage == percentage_or;
}

class AnglePercentage : public PercentageOr<Angle, AnglePercentage> {
public:
    using PercentageOr<Angle, AnglePercentage>::PercentageOr;

    bool is_angle() const { return is_t(); }
    Angle const& angle() const { return get_t(); }
};

class FrequencyPercentage : public PercentageOr<Frequency, FrequencyPercentage> {
public:
    using PercentageOr<Frequency, FrequencyPercentage>::PercentageOr;

    bool is_frequency() const { return is_t(); }
    Frequency const& frequency() const { return get_t(); }
};

class LengthPercentage : public PercentageOr<Length, LengthPercentage> {
public:
    using PercentageOr<Length, LengthPercentage>::PercentageOr;

    bool is_auto() const { return is_length() && length().is_auto(); }

    bool is_length() const { return is_t(); }
    Length const& length() const { return get_t(); }
};

class TimePercentage : public PercentageOr<Time, TimePercentage> {
public:
    using PercentageOr<Time, TimePercentage>::PercentageOr;

    bool is_time() const { return is_t(); }
    Time const& time() const { return get_t(); }
};

struct NumberPercentage : public PercentageOr<Number, NumberPercentage> {
public:
    using PercentageOr<Number, NumberPercentage>::PercentageOr;

    bool is_number() const { return is_t(); }
    Number const& number() const { return get_t(); }
};

}

template<>
struct AK::Formatter<Web::CSS::AnglePercentage> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::AnglePercentage const& angle_percentage)
    {
        return Formatter<StringView>::format(builder, angle_percentage.to_string(Web::CSS::SerializationMode::Normal));
    }
};

template<>
struct AK::Formatter<Web::CSS::FrequencyPercentage> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::FrequencyPercentage const& frequency_percentage)
    {
        return Formatter<StringView>::format(builder, frequency_percentage.to_string(Web::CSS::SerializationMode::Normal));
    }
};

template<>
struct AK::Formatter<Web::CSS::LengthPercentage> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::LengthPercentage const& length_percentage)
    {
        return Formatter<StringView>::format(builder, length_percentage.to_string(Web::CSS::SerializationMode::Normal));
    }
};

template<>
struct AK::Formatter<Web::CSS::TimePercentage> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::TimePercentage const& time_percentage)
    {
        return Formatter<StringView>::format(builder, time_percentage.to_string(Web::CSS::SerializationMode::Normal));
    }
};
