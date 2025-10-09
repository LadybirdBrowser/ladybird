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
        static Linear identity();

        struct Stop {
            double output;
            Optional<double> input;

            bool operator==(Stop const&) const = default;
        };

        Vector<Stop> stops;

        bool operator==(Linear const&) const = default;

        String to_string(SerializationMode) const;
    };

    struct CubicBezier {
        static CubicBezier ease();
        static CubicBezier ease_in();
        static CubicBezier ease_out();
        static CubicBezier ease_in_out();

        NumberOrCalculated x1 { 0 };
        NumberOrCalculated y1 { 0 };
        NumberOrCalculated x2 { 0 };
        NumberOrCalculated y2 { 0 };

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

        String to_string(SerializationMode) const;
    };

    struct Steps {
        static Steps step_start();
        static Steps step_end();

        IntegerOrCalculated number_of_intervals { 1 };
        StepPosition position { StepPosition::End };

        bool operator==(Steps const&) const = default;

        String to_string(SerializationMode) const;
    };

    struct WEB_API Function : public Variant<Linear, CubicBezier, Steps> {
        using Variant::Variant;

        String to_string(SerializationMode) const;
    };

    static ValueComparingNonnullRefPtr<EasingStyleValue const> create(Function const& function)
    {
        return adopt_ref(*new (nothrow) EasingStyleValue(function));
    }
    virtual ~EasingStyleValue() override = default;

    Function const& function() const { return m_function; }

    virtual String to_string(SerializationMode mode) const override { return m_function.to_string(mode); }

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
