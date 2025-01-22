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

template<typename T>
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

    virtual ~CalculatedOr() = default;

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
        if (is_calculated())
            return m_value.template get<NonnullRefPtr<CalculatedStyleValue>>()->to_string(Web::CSS::CSSStyleValue::SerializationMode::Normal);

        return m_value.template get<T>().to_string();
    }

    bool operator==(CalculatedOr<T> const& other) const
    {
        if (is_calculated() || other.is_calculated())
            return false;
        return (m_value.template get<T>() == other.m_value.template get<T>());
    }

protected:
    virtual Optional<T> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const = 0;
    virtual NonnullRefPtr<CSSStyleValue> create_style_value() const = 0;

private:
    Variant<T, NonnullRefPtr<CalculatedStyleValue>> m_value;
};

class AngleOrCalculated : public CalculatedOr<Angle> {
public:
    using CalculatedOr<Angle>::CalculatedOr;

private:
    virtual Optional<Angle> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CSSStyleValue> create_style_value() const override;
};

class FlexOrCalculated : public CalculatedOr<Flex> {
public:
    using CalculatedOr<Flex>::CalculatedOr;

private:
    virtual Optional<Flex> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CSSStyleValue> create_style_value() const override;
};

class FrequencyOrCalculated : public CalculatedOr<Frequency> {
public:
    using CalculatedOr<Frequency>::CalculatedOr;

private:
    virtual Optional<Frequency> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CSSStyleValue> create_style_value() const override;
};

class IntegerOrCalculated : public CalculatedOr<i64> {
public:
    using CalculatedOr<i64>::CalculatedOr;

private:
    virtual Optional<i64> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CSSStyleValue> create_style_value() const override;
};

class LengthOrCalculated : public CalculatedOr<Length> {
public:
    using CalculatedOr<Length>::CalculatedOr;

private:
    virtual Optional<Length> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CSSStyleValue> create_style_value() const override;
};

class NumberOrCalculated : public CalculatedOr<double> {
public:
    using CalculatedOr<double>::CalculatedOr;

private:
    virtual Optional<double> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CSSStyleValue> create_style_value() const override;
};

class PercentageOrCalculated : public CalculatedOr<Percentage> {
public:
    using CalculatedOr<Percentage>::CalculatedOr;

private:
    virtual Optional<Percentage> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CSSStyleValue> create_style_value() const override;
};

class ResolutionOrCalculated : public CalculatedOr<Resolution> {
public:
    using CalculatedOr<Resolution>::CalculatedOr;

private:
    virtual Optional<Resolution> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CSSStyleValue> create_style_value() const override;
};

class TimeOrCalculated : public CalculatedOr<Time> {
public:
    using CalculatedOr<Time>::CalculatedOr;

private:
    virtual Optional<Time> resolve_calculated(NonnullRefPtr<CalculatedStyleValue> const&, CalculationResolutionContext const&) const override;
    virtual NonnullRefPtr<CSSStyleValue> create_style_value() const override;
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
