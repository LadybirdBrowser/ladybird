/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/CSSCounterStyleRule.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/CSS/StyleValues/CounterStyleSystemStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSCounterStyleRule);

static Utf16String serialize_counter_style_descriptor(RefPtr<StyleValue const> const& descriptor)
{
    if (!descriptor)
        return {};

    return descriptor->to_utf16_string(SerializationMode::Normal);
}

GC::Ref<CSSCounterStyleRule> CSSCounterStyleRule::create(RustRule rule)
{
    return GC::Heap::the().allocate<CSSCounterStyleRule>(move(rule));
}

CSSCounterStyleRule::CSSCounterStyleRule(RustRule rule)
    : CSSRule(move(rule))
    , m_rule(*native_rule().payload().counter_style)
    , m_descriptors(Parser::ValueParserFFI::rust_counter_style_descriptors(&m_rule))
{
}

Utf16String CSSCounterStyleRule::serialized() const
{
    Utf16StringBuilder builder;
    builder.appendff("@counter-style {} {{", serialize_an_identifier(name()));

    for (auto id : { DescriptorID::System, DescriptorID::Negative, DescriptorID::Prefix, DescriptorID::Suffix, DescriptorID::Range, DescriptorID::Pad, DescriptorID::Fallback, DescriptorID::Symbols, DescriptorID::AdditiveSymbols, DescriptorID::SpeakAs }) {
        auto name = DescriptorNameAndID::from_id(id);
        if (auto value = m_descriptors.descriptor(name)) {
            builder.appendff(" {}: ", name.name());
            value->serialize(builder, SerializationMode::Normal);
            builder.append_ascii(';');
        }
    }

    builder.append_ascii(" }"sv);
    return builder.to_string();
}

Utf16View CSSCounterStyleRule::name() const
{
    auto view = Parser::ValueParserFFI::rust_counter_style_name(&m_rule);
    return { reinterpret_cast<char16_t const*>(view.utf16), view.length };
}

void CSSCounterStyleRule::set_name(Utf16String const& name)
{
    if (Parser::ValueParserFFI::rust_counter_style_set_name(&m_rule, Parser::ffi_utf16_view(name)))
        clear_caches();
}

Utf16String CSSCounterStyleRule::system() const
{
    return serialize_counter_style_descriptor(m_descriptors.descriptor(DescriptorNameAndID::from_id(DescriptorID::System)));
}

// https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-system
void CSSCounterStyleRule::set_system(Utf16String const& system)
{
    // 1. parse the given value as the descriptor associated with the attribute.
    Parser::ParsingParams parsing_params;
    auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, DescriptorNameAndID::from_id(CSS::DescriptorID::System), system);

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
    auto current_system = system_style_value();
    if (!current_system || current_system->as_counter_style_system().algorithm_differs_from(value->as_counter_style_system()))
        return;

    // 4. Set the descriptor to the value.
    m_descriptors.set(DescriptorNameAndID::from_id(DescriptorID::System), *value);

    clear_caches();
}

Utf16String CSSCounterStyleRule::negative() const
{
    return serialize_counter_style_descriptor(m_descriptors.descriptor(DescriptorNameAndID::from_id(DescriptorID::Negative)));
}

void CSSCounterStyleRule::set_negative(Utf16String const& negative)
{
    Parser::ParsingParams parsing_params;

    if (auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, DescriptorNameAndID::from_id(CSS::DescriptorID::Negative), negative)) {
        m_descriptors.set(DescriptorNameAndID::from_id(DescriptorID::Negative), *value);
        clear_caches();
    }
}

Utf16String CSSCounterStyleRule::prefix() const
{
    return serialize_counter_style_descriptor(m_descriptors.descriptor(DescriptorNameAndID::from_id(DescriptorID::Prefix)));
}

void CSSCounterStyleRule::set_prefix(Utf16String const& prefix)
{
    Parser::ParsingParams parsing_params;

    if (auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, DescriptorNameAndID::from_id(CSS::DescriptorID::Prefix), prefix)) {
        m_descriptors.set(DescriptorNameAndID::from_id(DescriptorID::Prefix), *value);
        clear_caches();
    }
}

Utf16String CSSCounterStyleRule::suffix() const
{
    return serialize_counter_style_descriptor(m_descriptors.descriptor(DescriptorNameAndID::from_id(DescriptorID::Suffix)));
}

void CSSCounterStyleRule::set_suffix(Utf16String const& suffix)
{
    Parser::ParsingParams parsing_params;

    if (auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, DescriptorNameAndID::from_id(CSS::DescriptorID::Suffix), suffix)) {
        m_descriptors.set(DescriptorNameAndID::from_id(DescriptorID::Suffix), *value);
        clear_caches();
    }
}

Utf16String CSSCounterStyleRule::range() const
{
    return serialize_counter_style_descriptor(m_descriptors.descriptor(DescriptorNameAndID::from_id(DescriptorID::Range)));
}

void CSSCounterStyleRule::set_range(Utf16String const& range)
{
    Parser::ParsingParams parsing_params;

    if (auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, DescriptorNameAndID::from_id(CSS::DescriptorID::Range), range)) {
        m_descriptors.set(DescriptorNameAndID::from_id(DescriptorID::Range), *value);
        clear_caches();
    }
}

Utf16String CSSCounterStyleRule::pad() const
{
    return serialize_counter_style_descriptor(m_descriptors.descriptor(DescriptorNameAndID::from_id(DescriptorID::Pad)));
}

void CSSCounterStyleRule::set_pad(Utf16String const& pad)
{
    Parser::ParsingParams parsing_params;

    if (auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, DescriptorNameAndID::from_id(CSS::DescriptorID::Pad), pad)) {
        m_descriptors.set(DescriptorNameAndID::from_id(DescriptorID::Pad), *value);
        clear_caches();
    }
}

Utf16String CSSCounterStyleRule::fallback() const
{
    return serialize_counter_style_descriptor(m_descriptors.descriptor(DescriptorNameAndID::from_id(DescriptorID::Fallback)));
}

void CSSCounterStyleRule::set_fallback(Utf16String const& fallback)
{
    Parser::ParsingParams parsing_params;

    if (auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, DescriptorNameAndID::from_id(CSS::DescriptorID::Fallback), fallback)) {
        m_descriptors.set(DescriptorNameAndID::from_id(DescriptorID::Fallback), *value);
        clear_caches();
    }
}

Utf16String CSSCounterStyleRule::symbols() const
{
    return serialize_counter_style_descriptor(m_descriptors.descriptor(DescriptorNameAndID::from_id(DescriptorID::Symbols)));
}

// https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-symbols
void CSSCounterStyleRule::set_symbols(Utf16String const& symbols)
{
    // On setting, run the following steps:

    // 1. parse the given value as the descriptor associated with the attribute.
    Parser::ParsingParams parsing_params;

    auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, DescriptorNameAndID::from_id(CSS::DescriptorID::Symbols), symbols);

    // 2. If the result is invalid according to the given descriptor’s grammar, or would cause the @counter-style rule
    //    to not define a counter style, do nothing and abort these steps. (For example, some systems require the
    //    symbols descriptor to contain two values.)
    auto current_system = system_style_value();
    if (!value || (current_system && !current_system->as_counter_style_system().is_valid_symbol_count(value->as_value_list().size())))
        return;

    // 3. If the attribute being set is system, and the new value would change the algorithm used, do nothing and abort
    //    these steps. It’s okay to change an aspect of the algorithm, like the first symbol value of a fixed system.

    // 4. Set the descriptor to the value.
    m_descriptors.set(DescriptorNameAndID::from_id(DescriptorID::Symbols), *value);

    clear_caches();
}

Utf16String CSSCounterStyleRule::additive_symbols() const
{
    return serialize_counter_style_descriptor(m_descriptors.descriptor(DescriptorNameAndID::from_id(DescriptorID::AdditiveSymbols)));
}

// https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-additivesymbols
void CSSCounterStyleRule::set_additive_symbols(Utf16String const& additive_symbols)
{
    // On setting, run the following steps:

    // 1. parse the given value as the descriptor associated with the attribute.
    Parser::ParsingParams parsing_params;

    auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, DescriptorNameAndID::from_id(CSS::DescriptorID::AdditiveSymbols), additive_symbols);

    // 2. If the result is invalid according to the given descriptor’s grammar, or would cause the @counter-style rule
    //    to not define a counter style, do nothing and abort these steps. (For example, some systems require the
    //    symbols descriptor to contain two values.)
    auto current_system = system_style_value();
    if (!value || (current_system && !current_system->as_counter_style_system().is_valid_additive_symbol_count(value->as_value_list().size())))
        return;

    // 3. If the attribute being set is system, and the new value would change the algorithm used, do nothing and abort
    //    these steps. It’s okay to change an aspect of the algorithm, like the first symbol value of a fixed system.

    // 4. Set the descriptor to the value.
    m_descriptors.set(DescriptorNameAndID::from_id(DescriptorID::AdditiveSymbols), *value);

    clear_caches();
}

Utf16String CSSCounterStyleRule::speak_as() const
{
    return serialize_counter_style_descriptor(m_descriptors.descriptor(DescriptorNameAndID::from_id(DescriptorID::SpeakAs)));
}

void CSSCounterStyleRule::set_speak_as(Utf16String const& speak_as)
{
    Parser::ParsingParams parsing_params;

    if (auto value = parse_css_descriptor(parsing_params, CSS::AtRuleID::CounterStyle, DescriptorNameAndID::from_id(CSS::DescriptorID::SpeakAs), speak_as)) {
        m_descriptors.set(DescriptorNameAndID::from_id(DescriptorID::SpeakAs), *value);
        clear_caches();
    }
}

void CSSCounterStyleRule::clear_caches()
{
    Base::clear_caches();

    auto* parent_style_sheet = this->parent_style_sheet();

    if (!parent_style_sheet)
        return;

    parent_style_sheet->for_each_owning_style_scope([&](StyleScope& style_scope) {
        style_scope.invalidate_counter_style_cache();
    });
    record_style_rule_declarations_changed(*this);
}

}
