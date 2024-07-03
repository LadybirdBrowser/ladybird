/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class EasingStyleValue final : public StyleValueWithDefaultOperators<EasingStyleValue> {
public:
    struct Linear {
        struct Stop {
            double offset;
            Optional<double> position;

            bool operator==(Stop const&) const = default;
        };

        Vector<Stop> stops;

        bool operator==(Linear const&) const = default;
    };

    struct CubicBezier {
        static CubicBezier ease();
        static CubicBezier ease_in();
        static CubicBezier ease_out();
        static CubicBezier ease_in_out();

        double x1;
        double y1;
        double x2;
        double y2;

        struct CachedSample {
            double x;
            double y;
            double t;
        };

        mutable Vector<CachedSample, 64> m_cached_x_samples {};

        bool operator==(CubicBezier const&) const;
    };

    struct Steps {
        enum class Position {
            JumpStart,
            JumpEnd,
            JumpNone,
            JumpBoth,
            Start,
            End,
        };

        static Steps step_start();
        static Steps step_end();

        unsigned int number_of_intervals;
        Position position { Position::End };

        bool operator==(Steps const&) const = default;
    };

    struct Function : public Variant<Linear, CubicBezier, Steps> {
        using Variant::Variant;

        double evaluate_at(double input_progress, bool before_flag) const;

        String to_string() const;
    };

    static ValueComparingNonnullRefPtr<EasingStyleValue> create(Function const& function)
    {
        return adopt_ref(*new (nothrow) EasingStyleValue(function));
    }
    virtual ~EasingStyleValue() override = default;

    Function const& function() const { return m_function; }

    virtual String to_string() const override { return m_function.to_string(); }

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
