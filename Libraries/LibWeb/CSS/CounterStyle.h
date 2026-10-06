/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibWeb/CSS/CounterStyleDefinition.h>
#include <LibWeb/CSS/Enums.h>

namespace Web::CSS::Parser::ValueParserFFI {

struct FfiRegisteredCounterStyle;

}

namespace Web::CSS {

// https://drafts.csswg.org/css-counter-styles-3/#counter-styles
class CounterStyle : public RefCounted<CounterStyle> {
public:
    static NonnullRefPtr<CounterStyle const> from_counter_style_definition(Layout::BegunRead const& read, CounterStyleDefinition const&, StyleScope const&);

    static NonnullRefPtr<CounterStyle const> create(Utf16FlyString name, CounterStyleAlgorithm algorithm, CounterStyleNegativeSign negative_sign, Utf16FlyString prefix, Utf16FlyString suffix, Vector<CounterStyleRangeEntry> range, Optional<Utf16FlyString> fallback, CounterStylePad pad)
    {
        // NB: All counter styles apart from 'decimal' must have a fallback.
        VERIFY(fallback.has_value() || name == "decimal"_utf16_fly_string);

        return adopt_ref(*new (nothrow) CounterStyle(move(name), move(algorithm), move(negative_sign), move(prefix), move(suffix), move(range), move(fallback), move(pad)));
    }

    Utf16FlyString const& name() const { return m_name; }
    CounterStyleAlgorithm const& algorithm() const { return m_algorithm; }
    CounterStyleNegativeSign const& negative_sign() const { return m_negative_sign; }
    Utf16FlyString const& prefix() const { return m_prefix; }
    Utf16FlyString const& suffix() const { return m_suffix; }
    Vector<CounterStyleRangeEntry> const& range() const { return m_range; }
    Optional<Utf16FlyString> const& fallback() const { return m_fallback; }
    CounterStylePad const& pad() const { return m_pad; }

    bool representation_is_constant() const;
    bool equals(CounterStyle const&) const;

    // The Rust counterpart of this style, which runs the representation algorithm and is what the style scope
    // publishes. It is made the first time it is asked for: a style scope rebuilding its counter styles resolves every one of them again and keeps the one it had
    // whenever the new one equals it.
    Parser::ValueParserFFI::FfiRegisteredCounterStyle const* rust_counter_style() const;

    virtual ~CounterStyle();

private:
    CounterStyle(Utf16FlyString name, CounterStyleAlgorithm algorithm, CounterStyleNegativeSign negative_sign, Utf16FlyString prefix, Utf16FlyString suffix, Vector<CounterStyleRangeEntry> range, Optional<Utf16FlyString> fallback, CounterStylePad pad);

    // Counter styles are composed of:
    // a name, to identify the style
    Utf16FlyString m_name;

    // an algorithm, which transforms integer counter values into a basic string representation
    CounterStyleAlgorithm m_algorithm;

    // a negative sign, which is prepended or appended to the representation of a negative counter value.
    CounterStyleNegativeSign m_negative_sign;

    // a prefix, to prepend to the representation
    Utf16FlyString m_prefix;

    // a suffix to append to the representation
    Utf16FlyString m_suffix;

    // a range, which limits the values that a counter style handles
    Vector<CounterStyleRangeEntry> m_range;

    // FIXME: a spoken form, which describes how to read out the counter style in a speech synthesizer

    // and a fallback style, to render the representation with when the counter value is outside the counter style’s
    // range or the counter style otherwise can’t render the counter value
    Optional<Utf16FlyString> m_fallback;

    // AD-HOC: We store the `pad` descriptor here as well to have everything in one place
    CounterStylePad m_pad;

    mutable Parser::ValueParserFFI::FfiRegisteredCounterStyle* m_rust_counter_style { nullptr };
};

// Whether the first three values this counter style represents are not all the same text: a marker whose text never
// changes (disc, circle, square, ...) reveals no renumbering.
bool counter_style_representation_depends_on_value(CounterStyle const&);

}
