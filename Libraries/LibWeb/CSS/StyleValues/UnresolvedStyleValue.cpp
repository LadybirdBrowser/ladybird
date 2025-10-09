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
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<UnresolvedStyleValue const> UnresolvedStyleValue::create(Vector<Parser::ComponentValue>&& values, Optional<Parser::SubstitutionFunctionsPresence> substitution_presence, Optional<String> original_source_text)
{
    if (!substitution_presence.has_value()) {
        substitution_presence = Parser::SubstitutionFunctionsPresence {};
        for (auto const& value : values) {
            if (value.is_function())
                value.function().contains_arbitrary_substitution_function(*substitution_presence);
            if (value.is_block())
                value.block().contains_arbitrary_substitution_function(*substitution_presence);
        }
    }

    return adopt_ref(*new (nothrow) UnresolvedStyleValue(move(values), *substitution_presence, move(original_source_text)));
}

UnresolvedStyleValue::UnresolvedStyleValue(Vector<Parser::ComponentValue>&& values, Parser::SubstitutionFunctionsPresence substitution_presence, Optional<String> original_source_text)
    : StyleValue(Type::Unresolved)
    , m_values(move(values))
    , m_substitution_functions_presence(substitution_presence)
    , m_original_source_text(move(original_source_text))
{
}

String UnresolvedStyleValue::to_string(SerializationMode) const
{
    if (m_original_source_text.has_value())
        return *m_original_source_text;

    return serialize_a_series_of_component_values(m_values, InsertWhitespace::Yes);
}

bool UnresolvedStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    return values() == other.as_unresolved().values();
}

static GC::Ref<CSSUnparsedValue> reify_a_list_of_component_values(JS::Realm&, Vector<Parser::ComponentValue> const&);

// https://drafts.css-houdini.org/css-typed-om-1/#reify-var
static GC::Root<CSSVariableReferenceValue> reify_a_var_reference(JS::Realm& realm, Parser::Function const& function)
{
    // NB: A var() might not be representable as a CSSVariableReferenceValue, for example if it has invalid syntax or
    //    it contains an ASF in its variable-name slot. In those cases, we return null here, so it's treated like a
    //    regular function.
    auto maybe_var_arguments = Parser::parse_according_to_argument_grammar(Parser::ArbitrarySubstitutionFunction::Var, function.value);
    if (!maybe_var_arguments.has_value())
        return nullptr;
    auto var_arguments = maybe_var_arguments.release_value();
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
    static Vector<GCRootCSSUnparsedSegment> reify(JS::Realm& realm, Vector<Parser::ComponentValue> const& source_values)
    {
        Reifier reifier;
        reifier.process_values(realm, source_values);
        if (!reifier.m_unserialized_values.is_empty())
            reifier.serialize_unserialized_values();
        return move(reifier.m_reified_values);
    }

private:
    void process_values(JS::Realm& realm, Vector<Parser::ComponentValue> const& source_values)
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
                    m_reified_values.append(move(var_reference));
                    continue;
                }
            }

            if (component_value.is_function()) {
                auto& function = component_value.function();
                m_unserialized_values.append(function.name_token);
                process_values(realm, function.value);
                m_unserialized_values.append(function.end_token);
                continue;
            }

            if (component_value.is_block()) {
                auto& block = component_value.block();
                m_unserialized_values.append(block.token);
                process_values(realm, block.value);
                m_unserialized_values.append(block.end_token);
                continue;
            }

            m_unserialized_values.append(component_value);
        }
    }

    void serialize_unserialized_values()
    {
        // FIXME: Stop inserting whitespace once we stop removing it during parsing.
        m_reified_values.append(serialize_a_series_of_component_values(m_unserialized_values, InsertWhitespace::Yes));
        m_unserialized_values.clear_with_capacity();
    }

    Vector<GCRootCSSUnparsedSegment> m_reified_values {};
    Vector<Parser::ComponentValue> m_unserialized_values {};
};

static GC::Ref<CSSUnparsedValue> reify_a_list_of_component_values(JS::Realm& realm, Vector<Parser::ComponentValue> const& component_values)
{
    // To reify a list of component values from a list:
    // 1. Replace all var() references in list with CSSVariableReferenceValue objects, as described in §5.4 var() References.
    // 2. Replace each remaining maximal subsequence of component values in list with a single string of their concatenated serializations.
    auto reified_values = Reifier::reify(realm, component_values);

    // 3. Return a new CSSUnparsedValue whose [[tokens]] slot is set to list.
    return CSSUnparsedValue::create(realm, move(reified_values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-list-of-component-values
GC::Ref<CSSStyleValue> UnresolvedStyleValue::reify(JS::Realm& realm, FlyString const&) const
{
    return reify_a_list_of_component_values(realm, m_values);
}

}
