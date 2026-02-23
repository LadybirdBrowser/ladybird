/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CounterStyleSystemStyleValue.h>

namespace Web::CSS {

struct CounterStyleRangeEntry {
    i64 start;
    i64 end;
};

// https://drafts.csswg.org/css-counter-styles-3/#counter-style-symbols
// <symbol> = <string> | <image> | <custom-ident>
// Note: The <image> syntax in <symbol> is currently at-risk. No implementations have plans to implement it currently,
//       and it complicates some usages of counter() in ways that haven’t been fully handled.
// FIXME: Given the above note we don't currently support <image> here - we may need to revisit this if other browsers
//        implement it.
using CounterStyleSymbol = FlyString;

struct CounterStyleNegativeSign {
    CounterStyleSymbol prefix;
    CounterStyleSymbol suffix;
};

struct CounterStylePad {
    i32 minimum_length;
    CounterStyleSymbol symbol;
};

struct AdditiveCounterStyleAlgorithm {
    struct AdditiveTuple {
        i32 weight;
        CounterStyleSymbol symbol;
    };
    Vector<AdditiveTuple> symbol_list;
};

struct FixedCounterStyleAlgorithm {
    i64 first_symbol;
    Vector<CounterStyleSymbol> symbol_list;
};

struct GenericCounterStyleAlgorithm {
    CounterStyleSystem type;
    Vector<CounterStyleSymbol> symbol_list;
};

using CounterStyleAlgorithm = Variant<AdditiveCounterStyleAlgorithm, FixedCounterStyleAlgorithm, GenericCounterStyleAlgorithm>;

struct AutoRange {
    static Vector<CounterStyleRangeEntry> resolve(CounterStyleAlgorithm const&);
};

class CounterStyleDefinition {
public:
    static CounterStyleDefinition create(FlyString name, Variant<CounterStyleAlgorithm, CounterStyleSystemStyleValue::Extends> algorithm, Optional<CounterStyleNegativeSign> negative_sign, Optional<CounterStyleSymbol> prefix, Optional<CounterStyleSymbol> suffix, Variant<Empty, AutoRange, Vector<CounterStyleRangeEntry>> range, Optional<FlyString> fallback, Optional<CounterStylePad> pad)
    {
        return CounterStyleDefinition(move(name), move(algorithm), move(negative_sign), move(prefix), move(suffix), move(range), move(fallback), move(pad));
    }

    static Optional<CounterStyleDefinition> from_counter_style_rule(CSSCounterStyleRule const&, ComputationContext const&);

    FlyString const& name() const { return m_name; }

    Variant<CounterStyleAlgorithm, CounterStyleSystemStyleValue::Extends> const& algorithm() const { return m_algorithm; }
    void set_algorithm(Variant<CounterStyleAlgorithm, CounterStyleSystemStyleValue::Extends> algorithm) { m_algorithm = move(algorithm); }

    Optional<CounterStyleNegativeSign> const& negative_sign() const { return m_negative_sign; }

    Optional<CounterStyleSymbol> const& prefix() const { return m_prefix; }

    Optional<CounterStyleSymbol> const& suffix() const { return m_suffix; }

    Variant<Empty, AutoRange, Vector<CounterStyleRangeEntry>> const& range() const { return m_range; }

    Optional<FlyString> const& fallback() const { return m_fallback; }

    Optional<CounterStylePad> const& pad() const { return m_pad; }

private:
    static Variant<Empty, CounterStyleAlgorithm, CounterStyleSystemStyleValue::Extends> resolve_algorithm(NonnullRefPtr<StyleValue const> const& system_style_value, RefPtr<StyleValue const> const& symbols_style_value, RefPtr<StyleValue const> const& additive_symbols_style_value, ComputationContext const&);
    static Vector<CounterStyleSymbol> resolve_symbols(NonnullRefPtr<StyleValue const> const& symbols_style_value);
    static Vector<AdditiveCounterStyleAlgorithm::AdditiveTuple> resolve_additive_symbols(NonnullRefPtr<StyleValue const> const& additive_symbols_style_value, ComputationContext const&);
    static CounterStyleNegativeSign resolve_negative_sign(NonnullRefPtr<StyleValue const> const&);
    static Variant<AutoRange, Vector<CounterStyleRangeEntry>> resolve_range(NonnullRefPtr<StyleValue const> const&, ComputationContext const&);
    static CounterStylePad resolve_pad(NonnullRefPtr<StyleValue const> const&, ComputationContext const&);

    CounterStyleDefinition(FlyString name, Variant<CounterStyleAlgorithm, CounterStyleSystemStyleValue::Extends> algorithm, Optional<CounterStyleNegativeSign> negative_sign, Optional<CounterStyleSymbol> prefix, Optional<CounterStyleSymbol> suffix, Variant<Empty, AutoRange, Vector<CounterStyleRangeEntry>> range, Optional<FlyString> fallback, Optional<CounterStylePad> pad)
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

    FlyString m_name;
    Variant<CounterStyleAlgorithm, CounterStyleSystemStyleValue::Extends> m_algorithm;
    Optional<CounterStyleNegativeSign> m_negative_sign;
    Optional<CounterStyleSymbol> m_prefix;
    Optional<CounterStyleSymbol> m_suffix;
    Variant<Empty, AutoRange, Vector<CounterStyleRangeEntry>> m_range;
    Optional<FlyString> m_fallback;
    Optional<CounterStylePad> m_pad;
};

}
