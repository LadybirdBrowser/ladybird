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

        void serialize(Utf16StringBuilder&, SerializationMode) const;
        String to_string(SerializationMode mode) const
        {
            return to_utf16_string(mode).to_utf8();
        }
        Utf16String to_utf16_string(SerializationMode mode) const
        {
            Utf16StringBuilder builder;
            serialize(builder, mode);
            return builder.to_string();
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

        void serialize(Utf16StringBuilder&, SerializationMode) const;
        String to_string(SerializationMode mode) const
        {
            return to_utf16_string(mode).to_utf8();
        }
        Utf16String to_utf16_string(SerializationMode mode) const
        {
            Utf16StringBuilder builder;
            serialize(builder, mode);
            return builder.to_string();
        }
    };

    struct Steps {
        ValueComparingNonnullRefPtr<StyleValue const> number_of_intervals;
        StepPosition position;

        bool operator==(Steps const&) const = default;

        void serialize(Utf16StringBuilder&, SerializationMode) const;
        String to_string(SerializationMode mode) const
        {
            return to_utf16_string(mode).to_utf8();
        }
        Utf16String to_utf16_string(SerializationMode mode) const
        {
            Utf16StringBuilder builder;
            serialize(builder, mode);
            return builder.to_string();
        }
    };

    struct WEB_API Function : public Variant<Linear, CubicBezier, Steps> {
        using Variant::Variant;

        void serialize(StringBuilder&, SerializationMode) const;
        void serialize(Utf16StringBuilder&, SerializationMode) const;
    };

    static ValueComparingNonnullRefPtr<EasingStyleValue const> create(Function const& function)
    {
        return adopt_ref(*new (nothrow) EasingStyleValue(function));
    }
    virtual ~EasingStyleValue() override = default;

    Function const& function() const;

    void serialize(StringBuilder& builder, SerializationMode mode) const { function().serialize(builder, mode); }
    void serialize(Utf16StringBuilder& builder, SerializationMode mode) const { function().serialize(builder, mode); }

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    bool properties_equal(EasingStyleValue const& other) const { return function() == other.function(); }

private:
    EasingStyleValue(Function const& function)
        : StyleValueWithDefaultOperators(Type::Easing, make_easing_data(function))
        , m_function(function)
    {
    }

    static StyleValueFFI::StyleValueData* make_easing_data(Function const&);

    // NB: The materialized function is a cache of the Rust-owned value data; it also carries the
    //     cubic-bezier sample cache. The Rust allocation stays authoritative.
    // NB: Eagerly materialized copy of the Rust-owned data, stored so function() can return a
    //     stable reference; immutable after construction, so sharing across style workers is
    //     safe (the cubic-bezier sample cache inside is only touched by main-thread animation
    //     evaluation).
    Function m_function;
};

}
