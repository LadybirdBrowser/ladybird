/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSCounterStyleRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSCounterStyleRule.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CounterStyleSystemStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSCounterStyleRule);

GC::Ref<CSSCounterStyleRule> CSSCounterStyleRule::create(JS::Realm& realm, FlyString name, RefPtr<StyleValue const> system, RefPtr<StyleValue const> negative, RefPtr<StyleValue const> prefix, RefPtr<StyleValue const> suffix, RefPtr<StyleValue const> range, RefPtr<StyleValue const> pad, RefPtr<StyleValue const> fallback, RefPtr<StyleValue const> symbols, RefPtr<StyleValue const> additive_symbols, RefPtr<StyleValue const> speak_as)
{
    return realm.create<CSSCounterStyleRule>(realm, name, move(system), move(negative), move(prefix), move(suffix), move(range), move(pad), move(fallback), move(symbols), move(additive_symbols), move(speak_as));
}

CSSCounterStyleRule::CSSCounterStyleRule(JS::Realm& realm, FlyString name, RefPtr<StyleValue const> system, RefPtr<StyleValue const> negative, RefPtr<StyleValue const> prefix, RefPtr<StyleValue const> suffix, RefPtr<StyleValue const> range, RefPtr<StyleValue const> pad, RefPtr<StyleValue const> fallback, RefPtr<StyleValue const> symbols, RefPtr<StyleValue const> additive_symbols, RefPtr<StyleValue const> speak_as)
    : CSSRule(realm, Type::CounterStyle)
    , m_name(move(name))
    , m_system(move(system))
    , m_negative(move(negative))
    , m_prefix(move(prefix))
    , m_suffix(move(suffix))
    , m_range(move(range))
    , m_pad(move(pad))
    , m_fallback(move(fallback))
    , m_symbols(move(symbols))
    , m_additive_symbols(move(additive_symbols))
    , m_speak_as(move(speak_as))
{
}

String CSSCounterStyleRule::serialized() const
{
    StringBuilder builder;
    builder.appendff("@counter-style {} {{", serialize_an_identifier(m_name));

    if (m_system) {
        builder.append(" system: "sv);
        m_system->serialize(builder, SerializationMode::Normal);
        builder.append(';');
    }

    if (m_negative) {
        builder.append(" negative: "sv);
        m_negative->serialize(builder, SerializationMode::Normal);
        builder.append(';');
    }

    if (m_prefix) {
        builder.append(" prefix: "sv);
        m_prefix->serialize(builder, SerializationMode::Normal);
        builder.append(';');
    }

    if (m_suffix) {
        builder.append(" suffix: "sv);
        m_suffix->serialize(builder, SerializationMode::Normal);
        builder.append(';');
    }

    if (m_range) {
        builder.append(" range: "sv);
        m_range->serialize(builder, SerializationMode::Normal);
        builder.append(';');
    }

    if (m_pad) {
        builder.append(" pad: "sv);
        m_pad->serialize(builder, SerializationMode::Normal);
        builder.append(';');
    }

    if (m_fallback) {
        builder.append(" fallback: "sv);
        m_fallback->serialize(builder, SerializationMode::Normal);
        builder.append(';');
    }

    if (m_symbols) {
        builder.append(" symbols: "sv);
        m_symbols->serialize(builder, SerializationMode::Normal);
        builder.append(';');
    }

    if (m_additive_symbols) {
        builder.append(" additive-symbols: "sv);
        m_additive_symbols->serialize(builder, SerializationMode::Normal);
        builder.append(';');
    }

    if (m_speak_as) {
        builder.append(" speak-as: "sv);
        m_speak_as->serialize(builder, SerializationMode::Normal);
        builder.append(';');
    }

    builder.append(" }"sv);
    return MUST(builder.to_string());
}

// https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-name
void CSSCounterStyleRule::set_name(FlyString name)
{
    // On setting the name attribute, run the following steps:

    // 1. If the value is an ASCII case-insensitive match for "none" or one of the non-overridable counter-style names, do nothing and return.
    if (name.equals_ignoring_ascii_case("none"sv) || matches_non_overridable_counter_style_name(name))
        return;

    // 2. If the value is an ASCII case-insensitive match for any of the predefined counter styles, lowercase it.
    if (auto keyword = keyword_from_string(name); keyword.has_value() && keyword_to_counter_style_name_keyword(keyword.release_value()).has_value())
        name = name.to_ascii_lowercase();

    // 3. Replace the associated rule’s name with an identifier equal to the value.
    m_name = move(name);
}

FlyString CSSCounterStyleRule::system() const
{
    if (!m_system)
        return ""_fly_string;

    return m_system->to_string(SerializationMode::Normal);
}

// https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-system
void CSSCounterStyleRule::set_system(FlyString const& system)
{
    // 1. parse the given value as the descriptor associated with the attribute.
    Parser::ParsingParams parsing_params { realm() };
    auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, CSS::DescriptorID::System, system);

    // 2. If the result is invalid according to the given descriptor’s grammar, or would cause the @counter-style rule
    //    to not define a counter style, do nothing and abort these steps. (For example, some systems require the
    //    symbols descriptor to contain two values.)
    // NB: Since we only allow changing parameters of the system, not the algorithm itself (see below), we know this
    //     change can't cause the @counter-style to not define a counter style.
    if (!value)
        return;

    // 3. If the attribute being set is system, and the new value would change the algorithm used, do nothing and abort
    //    these steps.
    // Note: It’s okay to change an aspect of the algorithm, like the first symbol value of a fixed system.
    if (!m_system || m_system->as_counter_style_system().algorithm_differs_from(value->as_counter_style_system()))
        return;

    // 4. Set the descriptor to the value.
    m_system = value;
}

FlyString CSSCounterStyleRule::negative() const
{
    if (!m_negative)
        return ""_fly_string;

    return m_negative->to_string(SerializationMode::Normal);
}

void CSSCounterStyleRule::set_negative(FlyString const& negative)
{
    Parser::ParsingParams parsing_params { realm() };

    if (auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, CSS::DescriptorID::Negative, negative))
        m_negative = value;
}

FlyString CSSCounterStyleRule::prefix() const
{
    if (!m_prefix)
        return ""_fly_string;

    return m_prefix->to_string(SerializationMode::Normal);
}

void CSSCounterStyleRule::set_prefix(FlyString const& prefix)
{
    Parser::ParsingParams parsing_params { realm() };

    if (auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, CSS::DescriptorID::Prefix, prefix))
        m_prefix = value;
}

FlyString CSSCounterStyleRule::suffix() const
{
    if (!m_suffix)
        return ""_fly_string;

    return m_suffix->to_string(SerializationMode::Normal);
}

void CSSCounterStyleRule::set_suffix(FlyString const& suffix)
{
    Parser::ParsingParams parsing_params { realm() };

    if (auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, CSS::DescriptorID::Suffix, suffix))
        m_suffix = value;
}

FlyString CSSCounterStyleRule::range() const
{
    if (!m_range)
        return ""_fly_string;

    return m_range->to_string(SerializationMode::Normal);
}

void CSSCounterStyleRule::set_range(FlyString const& range)
{
    Parser::ParsingParams parsing_params { realm() };

    if (auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, CSS::DescriptorID::Range, range))
        m_range = value;
}

FlyString CSSCounterStyleRule::pad() const
{
    if (!m_pad)
        return ""_fly_string;

    return m_pad->to_string(SerializationMode::Normal);
}

void CSSCounterStyleRule::set_pad(FlyString const& pad)
{
    Parser::ParsingParams parsing_params { realm() };

    if (auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, CSS::DescriptorID::Pad, pad))
        m_pad = value;
}

FlyString CSSCounterStyleRule::fallback() const
{
    if (!m_fallback)
        return ""_fly_string;

    return m_fallback->to_string(SerializationMode::Normal);
}

void CSSCounterStyleRule::set_fallback(FlyString const& fallback)
{
    Parser::ParsingParams parsing_params { realm() };

    if (auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, CSS::DescriptorID::Fallback, fallback))
        m_fallback = value;
}

FlyString CSSCounterStyleRule::symbols() const
{
    if (!m_symbols)
        return ""_fly_string;

    return m_symbols->to_string(SerializationMode::Normal);
}

// https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-symbols
void CSSCounterStyleRule::set_symbols(FlyString const& symbols)
{
    // On setting, run the following steps:

    // 1. parse the given value as the descriptor associated with the attribute.
    Parser::ParsingParams parsing_params { realm() };

    auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, CSS::DescriptorID::Symbols, symbols);

    // 2. If the result is invalid according to the given descriptor’s grammar, or would cause the @counter-style rule
    //    to not define a counter style, do nothing and abort these steps. (For example, some systems require the
    //    symbols descriptor to contain two values.)
    if (!value || (m_system && !m_system->as_counter_style_system().is_valid_symbol_count(value->as_value_list().size())))
        return;

    // 3. If the attribute being set is system, and the new value would change the algorithm used, do nothing and abort
    //    these steps. It’s okay to change an aspect of the algorithm, like the first symbol value of a fixed system.

    // 4. Set the descriptor to the value.
    m_symbols = value;
}

FlyString CSSCounterStyleRule::additive_symbols() const
{
    if (!m_additive_symbols)
        return ""_fly_string;

    return m_additive_symbols->to_string(SerializationMode::Normal);
}

// https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-additivesymbols
void CSSCounterStyleRule::set_additive_symbols(FlyString const& additive_symbols)
{
    // On setting, run the following steps:

    // 1. parse the given value as the descriptor associated with the attribute.
    Parser::ParsingParams parsing_params { realm() };

    auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, CSS::DescriptorID::AdditiveSymbols, additive_symbols);

    // 2. If the result is invalid according to the given descriptor’s grammar, or would cause the @counter-style rule
    //    to not define a counter style, do nothing and abort these steps. (For example, some systems require the
    //    symbols descriptor to contain two values.)
    if (!value || (m_system && !m_system->as_counter_style_system().is_valid_additive_symbol_count(value->as_value_list().size())))
        return;

    // 3. If the attribute being set is system, and the new value would change the algorithm used, do nothing and abort
    //    these steps. It’s okay to change an aspect of the algorithm, like the first symbol value of a fixed system.

    // 4. Set the descriptor to the value.
    m_additive_symbols = value;
}

FlyString CSSCounterStyleRule::speak_as() const
{
    if (!m_speak_as)
        return ""_fly_string;

    return m_speak_as->to_string(SerializationMode::Normal);
}

void CSSCounterStyleRule::set_speak_as(FlyString const& speak_as)
{
    Parser::ParsingParams parsing_params { realm() };

    if (auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, CSS::DescriptorID::SpeakAs, speak_as))
        m_speak_as = value;
}

void CSSCounterStyleRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSCounterStyleRule);
    Base::initialize(realm);
}

}
