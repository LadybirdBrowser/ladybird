/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Flex.h>
#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

template<typename Self, typename T>
class CalculatedOr {
public:
    CalculatedOr(T t)
        : m_value(move(t))
    {
    }

    CalculatedOr(NonnullRefPtr<CalculatedStyleValue> calculated)
        : m_value(move(calculated))
    {
    }

    bool is_calculated() const { return m_value.template has<NonnullRefPtr<CalculatedStyleValue>>(); }

    T const& value() const
    {
        VERIFY(!is_calculated());
        return m_value.template get<T>();
    }

    NonnullRefPtr<CSSStyleValue> as_style_value() const
    {
        if (is_calculated())
            return calculated();
        return create_style_value();
    }

    NonnullRefPtr<CalculatedStyleValue> const& calculated() const
    {
        VERIFY(is_calculated());
        return m_value.template get<NonnullRefPtr<CalculatedStyleValue>>();
    }

    Optional<T> resolved(CalculationResolutionContext const& context) const
    {
        return m_value.visit(
            [&](T const& t) -> Optional<T> {
                return t;
            },
            [&](NonnullRefPtr<CalculatedStyleValue> const& calculated) {
                return resolve_calculated(calculated, context);
            });
    }

    String to_string() const
    {
        return m_value.visit(
            [](T const& t) {
                if constexpr (IsArithmetic<T>) {
                    return String::number(t);
                } else {
                    return t.to_string();
                }
            },
            [](NonnullRefPtr<CalculatedStyleValue> const& calculated) {
                return calculated->to_string(CSSStyleValue::SerializationMode::Normal);
            });
    }

    bool operator==(CalculatedOr<Self, T> const& other) const
    {
        if (is_calculated() || other.is_calculated())
            return false;
        return (m_value.template get<T>() == other.m_value.template get<T>());
    }

protected:
    Optional<T> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const& calculated, CalculationResolutionContext const& context) const
    {
        return static_cast<Self const*>(this)->resolve_calculated(calculated, context);
    }
    NonnullRefPtr<CSSStyleValue> create_style_value() const
    {
        return static_cast<Self const*>(this)->create_style_value();
    }

private:
    Variant<T, NonnullRefPtr<CalculatedStyleValue>> m_value;
};

class AngleOrCalculated : public CalculatedOr<AngleOrCalculated, Angle> {
public:
    using CalculatedOr::CalculatedOr;

    Optional<Angle> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const;
    NonnullRefPtr<CSSStyleValue> create_style_value() const;
};

class FlexOrCalculated : public CalculatedOr<FlexOrCalculated, Flex> {
public:
    using CalculatedOr::CalculatedOr;

    Optional<Flex> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const;
    NonnullRefPtr<CSSStyleValue> create_style_value() const;
};

class FrequencyOrCalculated : public CalculatedOr<FrequencyOrCalculated, Frequency> {
public:
    using CalculatedOr::CalculatedOr;

    Optional<Frequency> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const;
    NonnullRefPtr<CSSStyleValue> create_style_value() const;
};

class IntegerOrCalculated : public CalculatedOr<IntegerOrCalculated, i64> {
public:
    using CalculatedOr::CalculatedOr;

    Optional<i64> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const;
    NonnullRefPtr<CSSStyleValue> create_style_value() const;
};

class LengthOrCalculated : public CalculatedOr<LengthOrCalculated, Length> {
public:
    using CalculatedOr::CalculatedOr;

    Optional<Length> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const;
    NonnullRefPtr<CSSStyleValue> create_style_value() const;
};

class NumberOrCalculated : public CalculatedOr<NumberOrCalculated, double> {
public:
    using CalculatedOr::CalculatedOr;

    Optional<double> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const;
    NonnullRefPtr<CSSStyleValue> create_style_value() const;
};

class PercentageOrCalculated : public CalculatedOr<PercentageOrCalculated, Percentage> {
public:
    using CalculatedOr::CalculatedOr;

    Optional<Percentage> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const;
    NonnullRefPtr<CSSStyleValue> create_style_value() const;
};

class ResolutionOrCalculated : public CalculatedOr<ResolutionOrCalculated, Resolution> {
public:
    using CalculatedOr::CalculatedOr;

    Optional<Resolution> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const;
    NonnullRefPtr<CSSStyleValue> create_style_value() const;
};

class TimeOrCalculated : public CalculatedOr<TimeOrCalculated, Time> {
public:
    using CalculatedOr::CalculatedOr;

    Optional<Time> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const;
    NonnullRefPtr<CSSStyleValue> create_style_value() const;
};

}

template<>
struct AK::Formatter<Web::CSS::AngleOrCalculated> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::AngleOrCalculated const& calculated_or)
    {
        return Formatter<StringView>::format(builder, calculated_or.to_string());
    }
};

template<>
struct AK::Formatter<Web::CSS::FrequencyOrCalculated> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::FrequencyOrCalculated const& calculated_or)
    {
        return Formatter<StringView>::format(builder, calculated_or.to_string());
    }
};

template<>
struct AK::Formatter<Web::CSS::LengthOrCalculated> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::LengthOrCalculated const& calculated_or)
    {
        return Formatter<StringView>::format(builder, calculated_or.to_string());
    }
};

template<>
struct AK::Formatter<Web::CSS::PercentageOrCalculated> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::PercentageOrCalculated const& calculated_or)
    {
        return Formatter<StringView>::format(builder, calculated_or.to_string());
    }
};

template<>
struct AK::Formatter<Web::CSS::TimeOrCalculated> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::TimeOrCalculated const& calculated_or)
    {
        return Formatter<StringView>::format(builder, calculated_or.to_string());
    }
};
