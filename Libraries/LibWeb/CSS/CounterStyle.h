/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/CounterStyleDefinition.h>
#include <LibWeb/CSS/Enums.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-counter-styles-3/#counter-styles
class CounterStyle : public RefCounted<CounterStyle> {
public:
    static NonnullRefPtr<CounterStyle const> decimal();
    static NonnullRefPtr<CounterStyle const> disc();
    static NonnullRefPtr<CounterStyle const> from_counter_style_definition(CounterStyleDefinition const&, HashMap<FlyString, NonnullRefPtr<CounterStyle const>> const&);

    static NonnullRefPtr<CounterStyle const> create(FlyString name, CounterStyleAlgorithm algorithm, CounterStyleNegativeSign negative_sign, FlyString prefix, FlyString suffix, Vector<CounterStyleRangeEntry> range, Optional<FlyString> fallback, CounterStylePad pad)
    {
        // NB: All counter styles apart from 'decimal' must have a fallback.
        VERIFY(fallback.has_value() || name == "decimal"_fly_string);

        return adopt_ref(*new (nothrow) CounterStyle(move(name), move(algorithm), move(negative_sign), move(prefix), move(suffix), move(range), move(fallback), move(pad)));
    }

    FlyString const& name() const { return m_name; }
    CounterStyleAlgorithm const& algorithm() const { return m_algorithm; }
    CounterStyleNegativeSign const& negative_sign() const { return m_negative_sign; }
    FlyString const& prefix() const { return m_prefix; }
    FlyString const& suffix() const { return m_suffix; }
    Vector<CounterStyleRangeEntry> const& range() const { return m_range; }
    Optional<FlyString> const& fallback() const { return m_fallback; }
    CounterStylePad const& pad() const { return m_pad; }

    Optional<String> generate_an_initial_representation_for_the_counter_value(i64 value) const;
    bool uses_a_negative_sign() const;

    virtual ~CounterStyle() = default;

private:
    CounterStyle(FlyString name, CounterStyleAlgorithm algorithm, CounterStyleNegativeSign negative_sign, FlyString prefix, FlyString suffix, Vector<CounterStyleRangeEntry> range, Optional<FlyString> fallback, CounterStylePad pad)
        : m_name(move(name))
        , m_algorithm(move(algorithm))
        , m_negative_sign(move(negative_sign))
        , m_prefix(move(prefix))
        , m_suffix(move(suffix))
        , m_range(move(range))
        , m_fallback(move(fallback))
        , m_pad(move(pad))
    {
    }

    // Counter styles are composed of:
    // a name, to identify the style
    FlyString m_name;

    // an algorithm, which transforms integer counter values into a basic string representation
    CounterStyleAlgorithm m_algorithm;

    // a negative sign, which is prepended or appended to the representation of a negative counter value.
    CounterStyleNegativeSign m_negative_sign;

    // a prefix, to prepend to the representation
    FlyString m_prefix;

    // a suffix to append to the representation
    FlyString m_suffix;

    // a range, which limits the values that a counter style handles
    Vector<CounterStyleRangeEntry> m_range;

    // FIXME: a spoken form, which describes how to read out the counter style in a speech synthesizer

    // and a fallback style, to render the representation with when the counter value is outside the counter style’s
    // range or the counter style otherwise can’t render the counter value
    Optional<FlyString> m_fallback;

    // AD-HOC: We store the `pad` descriptor here as well to have everything in one place
    CounterStylePad m_pad;
};

String generate_a_counter_representation(RefPtr<CounterStyle const> const& counter_style, HashMap<FlyString, NonnullRefPtr<CounterStyle const>> const& registered_counter_styles, i32 value);

}
