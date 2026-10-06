/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterStyle.h"
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS {

NonnullRefPtr<CounterStyle const> CounterStyle::from_counter_style_definition(Layout::BegunRead const& read, CounterStyleDefinition const& definition, StyleScope const& style_scope)
{
    return definition.algorithm().visit(
        [&](CounterStyleSystemStyleValue::Extends const& extends) {
            // NB: The caller should ensure that any dependencies (i.e. counter styles that occur in the extends chain)
            //     of this counter style are registered before this counter style.
            auto extended_counter_style = style_scope.get_registered_counter_style(read, extends.name);

            if (!extended_counter_style)
                extended_counter_style = style_scope.get_registered_counter_style(read, "decimal"_utf16_fly_string);

            return CounterStyle::create(
                definition.name(),
                extended_counter_style->algorithm(),
                definition.negative_sign().value_or(extended_counter_style->negative_sign()),
                definition.prefix().value_or(extended_counter_style->prefix()),
                definition.suffix().value_or(extended_counter_style->suffix()),
                definition.range().visit(
                    [&](Empty const&) { return extended_counter_style->range(); },
                    [](Vector<CounterStyleRangeEntry> const& range) { return range; },
                    [&](AutoRange const&) { return AutoRange::resolve(extended_counter_style->algorithm()); }),
                definition.fallback().value_or(extended_counter_style->fallback().value_or("decimal"_utf16_fly_string)),
                definition.pad().value_or(extended_counter_style->pad()));
        },
        [&](CounterStyleAlgorithm const& algorithm) {
            return CounterStyle::create(
                definition.name(),
                algorithm,
                definition.negative_sign().value_or({ .prefix = "-"_utf16_fly_string, .suffix = ""_utf16_fly_string }),
                definition.prefix().value_or(""_utf16_fly_string), definition.suffix().value_or(". "_utf16_fly_string),
                definition.range().visit(
                    [](Vector<CounterStyleRangeEntry> const& range) { return range; },
                    [&](auto const&) { return AutoRange::resolve(algorithm); }),
                definition.fallback().value_or("decimal"_utf16_fly_string),
                definition.pad().value_or({ .minimum_length = 0, .symbol = ""_utf16_fly_string }));
        });
}

bool CounterStyle::equals(CounterStyle const& other) const
{
    return name() == other.name()
        && algorithm() == other.algorithm()
        && negative_sign() == other.negative_sign()
        && prefix() == other.prefix()
        && suffix() == other.suffix()
        && range() == other.range()
        && pad() == other.pad()
        && fallback() == other.fallback();
}

bool CounterStyle::representation_is_constant() const
{
    auto const* generic_algorithm = m_algorithm.get_pointer<GenericCounterStyleAlgorithm>();
    if (!generic_algorithm || generic_algorithm->type != CounterStyleSystem::Cyclic || generic_algorithm->symbol_list.size() != 1)
        return false;
    return m_range.size() == 1
        && m_range.first().start == NumericLimits<i32>::min()
        && m_range.first().end == NumericLimits<i32>::max();
}

// The descriptors travel as flat columns of `AK::Utf16FlyString` raw words; the Rust side takes ownership of the one
// reference each leaked word carries.
static Parser::ValueParserFFI::FfiRegisteredCounterStyle* create_rust_counter_style(Utf16FlyString const& name, CounterStyleAlgorithm const& algorithm, CounterStyleNegativeSign const& negative_sign, Utf16FlyString const& prefix, Utf16FlyString const& suffix, Vector<CounterStyleRangeEntry> const& range, Optional<Utf16FlyString> const& fallback, CounterStylePad const& pad)
{
    Vector<size_t> symbols;
    Vector<i32> additive_weights;
    u8 algorithm_kind = 0;
    u8 generic_system = 0;
    u8 extended_cjk_style = 0;
    i32 fixed_first_symbol = 0;

    algorithm.visit(
        [&](AdditiveCounterStyleAlgorithm const& additive) {
            algorithm_kind = 0;
            symbols.ensure_capacity(additive.symbol_list.size());
            additive_weights.ensure_capacity(additive.symbol_list.size());
            for (auto const& tuple : additive.symbol_list) {
                symbols.unchecked_append(tuple.symbol.to_raw_leaked());
                additive_weights.unchecked_append(tuple.weight);
            }
        },
        [&](FixedCounterStyleAlgorithm const& fixed) {
            algorithm_kind = 1;
            fixed_first_symbol = fixed.first_symbol;
            for (auto const& symbol : fixed.symbol_list)
                symbols.append(symbol.to_raw_leaked());
        },
        [&](GenericCounterStyleAlgorithm const& generic) {
            algorithm_kind = 2;
            generic_system = to_underlying(generic.type);
            for (auto const& symbol : generic.symbol_list)
                symbols.append(symbol.to_raw_leaked());
        },
        [&](EthiopicNumericCounterStyleAlgorithm const&) {
            algorithm_kind = 3;
        },
        [&](ExtendedCJKCounterStyleAlgorithm const& extended_cjk) {
            algorithm_kind = 4;
            extended_cjk_style = to_underlying(extended_cjk.type);
        });

    // The discriminants cross as raw codes; the Rust side depends on them.
    static_assert(to_underlying(CounterStyleSystem::Cyclic) == 0);
    static_assert(to_underlying(CounterStyleSystem::Numeric) == 1);
    static_assert(to_underlying(CounterStyleSystem::Alphabetic) == 2);
    static_assert(to_underlying(CounterStyleSystem::Symbolic) == 3);
    static_assert(to_underlying(ExtendedCJKCounterStyleAlgorithm::Type::SimpChineseInformal) == 0);
    static_assert(to_underlying(ExtendedCJKCounterStyleAlgorithm::Type::KoreanHanjaFormal) == 8);

    Vector<Parser::ValueParserFFI::FfiCounterStyleRange> ranges;
    ranges.ensure_capacity(range.size());
    for (auto const& entry : range)
        ranges.unchecked_append({ .start = entry.start, .end = entry.end });

    Parser::ValueParserFFI::FfiCounterStyleDescriptors descriptors {
        .name = name.to_raw_leaked(),
        .algorithm_kind = algorithm_kind,
        .generic_system = generic_system,
        .extended_cjk_style = extended_cjk_style,
        .fixed_first_symbol = fixed_first_symbol,
        .symbols = symbols.data(),
        .symbol_count = symbols.size(),
        .additive_weights = additive_weights.data(),
        .ranges = ranges.data(),
        .range_count = ranges.size(),
        .negative_prefix = negative_sign.prefix.to_raw_leaked(),
        .negative_suffix = negative_sign.suffix.to_raw_leaked(),
        .prefix = prefix.to_raw_leaked(),
        .suffix = suffix.to_raw_leaked(),
        .fallback = fallback.has_value() ? fallback->to_raw_leaked() : 0,
        .pad_symbol = pad.symbol.to_raw_leaked(),
        .pad_minimum_length = pad.minimum_length,
    };
    return Parser::ValueParserFFI::rust_counter_style_create(descriptors);
}

CounterStyle::CounterStyle(Utf16FlyString name, CounterStyleAlgorithm algorithm, CounterStyleNegativeSign negative_sign, Utf16FlyString prefix, Utf16FlyString suffix, Vector<CounterStyleRangeEntry> range, Optional<Utf16FlyString> fallback, CounterStylePad pad)
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

CounterStyle::~CounterStyle()
{
    Parser::ValueParserFFI::rust_counter_style_release(m_rust_counter_style);
}

Parser::ValueParserFFI::FfiRegisteredCounterStyle const* CounterStyle::rust_counter_style() const
{
    if (!m_rust_counter_style)
        m_rust_counter_style = create_rust_counter_style(m_name, m_algorithm, m_negative_sign, m_prefix, m_suffix, m_range, m_fallback, m_pad);
    return m_rust_counter_style;
}

bool counter_style_representation_depends_on_value(CounterStyle const& counter_style)
{
    return Parser::ValueParserFFI::rust_counter_style_representation_depends_on_value(counter_style.rust_counter_style());
}

}
