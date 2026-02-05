/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Root.h>
#include <LibWeb/Forward.h>

namespace Web::Animations {

struct TimeValue {
    static TimeValue from_css_numberish(CSS::CSSNumberish const&, DOM::AbstractElement const&);
    static TimeValue create_zero(GC::Ptr<AnimationTimeline> const& timeline)
    {
        // FIXME: Return 0% rather than 0ms for progress based timelines
        (void)timeline;

        return TimeValue { Type::Milliseconds, 0.0 };
    }

    enum class Type : u8 {
        Milliseconds,
        // FIXME: Support percentages
    };
    Type type;
    double value;

    TimeValue operator-() const
    {
        return { type, -value };
    }

    TimeValue operator*(double other) const
    {
        return { type, value * other };
    }

    TimeValue operator-(TimeValue const& other) const
    {
        VERIFY(type == other.type);
        return { type, value - other.value };
    }

    TimeValue operator+(TimeValue const& other) const
    {
        VERIFY(type == other.type);
        return { type, value + other.value };
    }

    TimeValue operator/(double divisor) const
    {
        return { type, value / divisor };
    }

    double operator/(TimeValue const& other) const
    {
        VERIFY(type == other.type);
        return value / other.value;
    }

    int operator<=>(TimeValue const& other) const
    {
        VERIFY(type == other.type);

        if (value < other.value)
            return -1;
        if (value > other.value)
            return 1;
        return 0;
    }

    bool operator==(TimeValue const& other) const
    {
        return type == other.type && value == other.value;
    }

    CSS::CSSNumberish as_css_numberish() const
    {
        switch (type) {
        case Type::Milliseconds:
            return value;
        }

        VERIFY_NOT_REACHED();
    }
};

// FIXME: This struct is required since our IDL generator requires us to return nullable union types as
//        Variant<Empty, Ts...> rather than Optional<Variant<Ts...>> (although setters are forced to be
//        Optional<Variant<Ts...>>)
struct NullableCSSNumberish : FlattenVariant<Variant<Empty>, CSS::CSSNumberish> {
    using Variant::Variant;

    static NullableCSSNumberish from_optional_css_numberish_time(Optional<TimeValue> const& value)
    {
        if (value.has_value())
            return value->as_css_numberish();

        return {};
    }
};

}

template<>
struct AK::Formatter<Web::Animations::TimeValue> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Web::Animations::TimeValue const& time)
    {
        switch (time.type) {
        case Web::Animations::TimeValue::Type::Milliseconds:
            return Formatter<FormatString>::format(builder, "{}ms"sv, time.value);
        }
        return {};
    }
};
