/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/CSS/CSSUnparsedValue.h>
#include <LibWeb/CSS/CSSVariableReferenceValue.h>
#include <LibWeb/CSS/Parser/ArbitrarySubstitutionFunctions.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/TokenStream.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>

namespace Web::CSS {

static String source_text_from_component_values(Vector<Parser::ComponentValue> const& values, UnresolvedStyleValue::SourceTextMode source_text_mode)
{
    StringBuilder builder;
    for (auto const& value : values) {
        auto original_source_text = value.original_source_text();
        if (original_source_text.is_empty()) {
            auto serialized_values = serialize_a_series_of_component_values(values);
            if (source_text_mode == UnresolvedStyleValue::SourceTextMode::Trim)
                return MUST(serialized_values.trim_ascii_whitespace());
            return serialized_values;
        }
        builder.append(original_source_text);
    }

    auto source_text = builder.to_string_without_validation();
    if (source_text_mode == UnresolvedStyleValue::SourceTextMode::Trim)
        return MUST(source_text.trim_ascii_whitespace());
    return source_text;
}

static void mark_as_attr_tainted(Vector<Parser::ComponentValue>& values)
{
    for (auto& value : values)
        value.set_attr_tainted();
}

StringView UnresolvedStyleValue::comparison_text() const
{
    if (!m_value_comparison_text.is_empty())
        return m_value_comparison_text.bytes_as_string_view();
    return m_source_text.bytes_as_string_view().trim_whitespace();
}

ValueComparingNonnullRefPtr<UnresolvedStyleValue const> UnresolvedStyleValue::create(Vector<Parser::ComponentValue>&& values, Parser::SubstitutionFunctionsPresence substitution_presence, Optional<String> original_source_text, SourceTextMode source_text_mode, bool contains_attr_tainted_values)
{
    auto has_original_source_text = original_source_text.has_value();
    auto source_text = [&] {
        if (has_original_source_text)
            return MUST(original_source_text.release_value().trim_ascii_whitespace());

        if (source_text_mode == SourceTextMode::Trim)
            return MUST(serialize_a_series_of_component_values_preserving_original_source_text(values).trim_ascii_whitespace());

        return source_text_from_component_values(values, source_text_mode);
    }();
    // NB: The comparison text is a normalized serialization, only used when we have separate original source text.
    //     Don't pay for serializing it otherwise.
    auto value_comparison_text = has_original_source_text ? MUST(serialize_a_series_of_component_values(values).trim_ascii_whitespace()) : String {};
    return adopt_ref(*new (nothrow) UnresolvedStyleValue(move(source_text), move(value_comparison_text), substitution_presence, contains_attr_tainted_values));
}

UnresolvedStyleValue::UnresolvedStyleValue(String source_text, String value_comparison_text, Parser::SubstitutionFunctionsPresence substitution_presence, bool contains_attr_tainted_values)
    : StyleValue(Type::Unresolved)
    , m_source_text(move(source_text))
    , m_value_comparison_text(move(value_comparison_text))
    , m_substitution_functions_presence(substitution_presence)
    , m_contains_attr_tainted_values(contains_attr_tainted_values)
{
}

void UnresolvedStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    builder.append(m_source_text);
}

Vector<Parser::ComponentValue> UnresolvedStyleValue::values() const
{
    auto parser = Parser::Parser::create(Parser::ParsingParams {}, m_value_comparison_text.is_empty() ? m_source_text : m_value_comparison_text);
    auto values = parser.parse_as_list_of_component_values();
    if (m_contains_attr_tainted_values)
        mark_as_attr_tainted(values);
    return values;
}

Vector<Parser::ComponentValue> UnresolvedStyleValue::tokenize() const
{
    return values();
}

bool UnresolvedStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;

    auto const& other_unresolved = other.as_unresolved();
    return comparison_text() == other_unresolved.comparison_text();
}

static GC::Ref<CSSUnparsedValue> reify_a_list_of_component_values(JS::Realm&, ReadonlySpan<Parser::ComponentValue>);

// https://drafts.css-houdini.org/css-typed-om-1/#reify-var
static GC::Ptr<CSSVariableReferenceValue> reify_a_var_reference(JS::Realm& realm, Parser::Function function)
{
    // NB: A var() might not be representable as a CSSVariableReferenceValue, for example if it has invalid syntax or
    //    it contains an ASF in its variable-name slot. In those cases, we return null here, so it's treated like a
    //    regular function.
    auto maybe_var_arguments = Parser::parse_according_to_argument_grammar(Parser::ArbitrarySubstitutionFunction::Var, function.value);
    if (!maybe_var_arguments.has_value())
        return nullptr;
    auto var_arguments = maybe_var_arguments.release_value().get<Parser::DeclarationValueList>();
    // NB: Try to parse the variable name. If we can't, return null as above.

    Parser::TokenStream tokens { var_arguments.first() };
    tokens.discard_whitespace();
    auto& maybe_variable = tokens.consume_a_token();
    tokens.discard_whitespace();
    if (tokens.has_next_token()
        || !maybe_variable.is(Parser::Token::Type::Ident)
        || !is_a_custom_property_name_string(maybe_variable.token().ident()))
        return nullptr;

    // To reify a var() reference var:
    // 1. Let object be a new CSSVariableReferenceValue.

    // 2. Set object’s variable internal slot to the serialization of the <custom-ident> providing the variable name.
    FlyString variable = maybe_variable.token().ident();

    // 3. If var has a fallback value, set object’s fallback internal slot to the result of reifying the fallback’s
    //    component values. Otherwise, set it to null.
    GC::Ptr<CSSUnparsedValue> fallback;
    if (var_arguments.size() > 1)
        fallback = reify_a_list_of_component_values(realm, var_arguments[1]);

    // 4. Return object.
    return CSSVariableReferenceValue::create(realm, move(variable), move(fallback));
}

class Reifier {
public:
    static Vector<CSSUnparsedSegment> reify(JS::Realm& realm, ReadonlySpan<Parser::ComponentValue> source_values)
    {
        Reifier reifier;
        reifier.process_values(realm, source_values);
        if (!reifier.m_unserialized_values.is_empty())
            reifier.serialize_unserialized_values();
        return move(reifier.m_reified_values);
    }

private:
    void process_values(JS::Realm& realm, ReadonlySpan<Parser::ComponentValue> source_values)
    {
        // NB: var() could be arbitrarily nested within other functions and blocks, so we have to walk the tree.
        //     Also, a var() might not be representable, if it has an ASF in place of its name, so those will be part
        //     of a string instead.
        for (auto const& component_value : source_values) {
            if (component_value.is_function("var"sv)) {
                // First parse the var() to see if it is representable as a CSSVariableReferenceValue. It might not be,
                // for example if it has an ASF in the place of its variable name. In that case we fall back to
                // serializing it like a regular function.
                if (auto var_reference = reify_a_var_reference(realm, component_value.function())) {
                    serialize_unserialized_values();
                    m_reified_values.append(GC::Ref { *var_reference });
                    continue;
                }
            }

            if (component_value.is_function()) {
                auto& function = component_value.function();
                m_unserialized_values.append(Parser::Token::create_function(function.name, function.name_token.original_source_text()));
                process_values(realm, function.value);
                m_unserialized_values.append(Parser::Token::create(function.end_token.type(), function.end_token.original_source_text()));
                continue;
            }

            if (component_value.is_block()) {
                auto& block = component_value.block();
                m_unserialized_values.append(Parser::Token::create(block.token.type(), block.token.original_source_text()));
                process_values(realm, block.value);
                m_unserialized_values.append(Parser::Token::create(block.end_token.type(), block.end_token.original_source_text()));
                continue;
            }

            m_unserialized_values.append(component_value);
        }
    }

    void serialize_unserialized_values()
    {
        m_reified_values.append(serialize_a_series_of_component_values(m_unserialized_values));
        m_unserialized_values.clear_with_capacity();
    }

    Vector<CSSUnparsedSegment> m_reified_values {};
    Vector<Parser::ComponentValue> m_unserialized_values {};
};

static GC::Ref<CSSUnparsedValue> reify_a_list_of_component_values(JS::Realm& realm, ReadonlySpan<Parser::ComponentValue> component_values)
{
    // To reify a list of component values from a list:
    // 1. Replace all var() references in list with CSSVariableReferenceValue objects, as described in §5.4 var() References.
    // 2. Replace each remaining maximal subsequence of component values in list with a single string of their concatenated serializations.
    auto reified_values = Reifier::reify(realm, component_values);

    // 3. Return a new CSSUnparsedValue whose [[tokens]] slot is set to list.
    return CSSUnparsedValue::create(realm, move(reified_values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-list-of-component-values
GC::Ref<CSSStyleValue> UnresolvedStyleValue::reify(JS::Realm& realm, Utf16FlyString const&) const
{
    auto component_values = values();
    return reify_a_list_of_component_values(realm, component_values);
}

}
