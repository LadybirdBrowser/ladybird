/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CalculatedOr.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class EasingStyleValue final : public StyleValueWithDefaultOperators<EasingStyleValue> {
public:
    struct Linear {
        struct Stop {
            ValueComparingNonnullRefPtr<StyleValue const> output;
            ValueComparingRefPtr<StyleValue const> input;

            bool operator==(Stop const&) const = default;
        };

        Vector<Stop> stops;

        bool operator==(Linear const&) const = default;

        void serialize(StringBuilder&, SerializationMode) const;
        String to_string(SerializationMode mode) const
        {
            StringBuilder builder;
            serialize(builder, mode);
            return builder.to_string_without_validation();
        }
    };

    struct CubicBezier {
        ValueComparingNonnullRefPtr<StyleValue const> x1;
        ValueComparingNonnullRefPtr<StyleValue const> y1;
        ValueComparingNonnullRefPtr<StyleValue const> x2;
        ValueComparingNonnullRefPtr<StyleValue const> y2;

        struct CachedSample {
            double x;
            double y;
            double t;
        };

        mutable Vector<CachedSample> m_cached_x_samples {};

        bool operator==(CubicBezier const& other) const
        {
            return x1 == other.x1 && y1 == other.y1 && x2 == other.x2 && y2 == other.y2;
        }

        void serialize(StringBuilder&, SerializationMode) const;
        String to_string(SerializationMode mode) const
        {
            StringBuilder builder;
            serialize(builder, mode);
            return builder.to_string_without_validation();
        }
    };

    struct Steps {
        ValueComparingNonnullRefPtr<StyleValue const> number_of_intervals;
        StepPosition position;

        bool operator==(Steps const&) const = default;

        void serialize(StringBuilder&, SerializationMode) const;
        String to_string(SerializationMode mode) const
        {
            StringBuilder builder;
            serialize(builder, mode);
            return builder.to_string_without_validation();
        }
    };

    struct WEB_API Function : public Variant<Linear, CubicBezier, Steps> {
        using Variant::Variant;

        void serialize(StringBuilder&, SerializationMode) const;
    };

    static ValueComparingNonnullRefPtr<EasingStyleValue const> create(Function const& function)
    {
        return adopt_ref(*new (nothrow) EasingStyleValue(function));
    }
    virtual ~EasingStyleValue() override = default;

    Function const& function() const { return m_function; }

    virtual void serialize(StringBuilder& builder, SerializationMode mode) const override { m_function.serialize(builder, mode); }

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    bool properties_equal(EasingStyleValue const& other) const { return m_function == other.m_function; }

private:
    EasingStyleValue(Function const& function)
        : StyleValueWithDefaultOperators(Type::Easing)
        , m_function(function)
    {
    }

    Function m_function;
};

}
