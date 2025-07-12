/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/ArbitrarySubstitutionFunctions.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS::Parser {

bool SubstitutionContext::operator==(SubstitutionContext const& other) const
{
    return dependency_type == other.dependency_type
        && first == other.first
        && second == other.second;
}

String SubstitutionContext::to_string() const
{
    StringView type_name = [this] {
        switch (dependency_type) {
        case DependencyType::Property:
            return "Property"sv;
        case DependencyType::Attribute:
            return "Attribute"sv;
        case DependencyType::Function:
            return "Function"sv;
        }
        VERIFY_NOT_REACHED();
    }();
    return MUST(String::formatted("{} {} {}", type_name, first, second));
}

void GuardedSubstitutionContexts::guard(SubstitutionContext& context)
{
    for (auto& existing_context : m_contexts) {
        if (existing_context == context) {
            existing_context.is_cyclic = true;
            context.is_cyclic = true;
            return;
        }
    }

    m_contexts.append(context);
}

void GuardedSubstitutionContexts::unguard(SubstitutionContext const& context)
{
    [[maybe_unused]] auto const was_removed = m_contexts.remove_first_matching([context](auto const& other) {
        return context == other;
    });
    VERIFY(was_removed);
}

Optional<ArbitrarySubstitutionFunction> to_arbitrary_substitution_function(FlyString const& name)
{
    if (name.equals_ignoring_ascii_case("attr"sv))
        return ArbitrarySubstitutionFunction::Attr;
    if (name.equals_ignoring_ascii_case("var"sv))
        return ArbitrarySubstitutionFunction::Var;
    return {};
}

bool contains_guaranteed_invalid_value(Vector<ComponentValue> const& values)
{
    for (auto const& value : values) {
        if (value.contains_guaranteed_invalid_value())
            return true;
    }
    return false;
}

// https://drafts.csswg.org/css-values-5/#replace-an-attr-function
static Vector<ComponentValue> replace_an_attr_function(DOM::AbstractElement& element, GuardedSubstitutionContexts& guarded_contexts, ArbitrarySubstitutionFunctionArguments const& arguments)
{
    // 1. Let el be the element that the style containing the attr() function is being applied to.
    //    Let first arg be the first <declaration-value> in arguments.
    //    Let second arg be the <declaration-value>? passed after the comma, or null if there was no comma.
    auto const& first_argument = arguments.first();
    auto const second_argument = arguments.get(1);

    FlyString attribute_name;
    Optional<FlyString> maybe_syntax = {};
    auto failure = [&] -> Vector<ComponentValue> {
        // This is step 6, but defined here for convenience.

        // 1. If second arg is null, and syntax was omitted, return an empty CSS <string>.
        if (!second_argument.has_value() && !maybe_syntax.has_value())
            return { Token::create_string({}) };

        // 2. If second arg is null, return the guaranteed-invalid value.
        if (!second_argument.has_value())
            return { ComponentValue { GuaranteedInvalidValue {} } };

        // 3. Substitute arbitrary substitution functions in second arg, and return the result.
        return substitute_arbitrary_substitution_functions(element, guarded_contexts, second_argument.value());
    };

    // 2. Substitute arbitrary substitution functions in first arg, then parse it as <attr-name> <attr-type>?.
    //    If that returns failure, jump to the last step (labeled FAILURE).
    //    Otherwise, let attr name and syntax be the results of parsing (with syntax being null if <attr-type> was
    //    omitted), processed as specified in the definition of those arguments.
    auto substituted = substitute_arbitrary_substitution_functions(element, guarded_contexts, first_argument);
    TokenStream first_argument_tokens { substituted };
    // <attr-name> = [ <ident-token>? '|' ]? <ident-token>
    // FIXME: Support optional attribute namespace
    if (!first_argument_tokens.next_token().is(Token::Type::Ident))
        return failure();
    attribute_name = first_argument_tokens.consume_a_token().token().ident();
    first_argument_tokens.discard_whitespace();

    // <attr-type> = type( <syntax> ) | raw-string | <attr-unit>
    // FIXME: Support type(<syntax>)
    bool is_dimension_unit = false;
    if (first_argument_tokens.next_token().is(Token::Type::Ident)) {
        auto const& syntax_ident = first_argument_tokens.next_token().token().ident();
        if (syntax_ident.equals_ignoring_ascii_case("raw-string"sv)) {
            maybe_syntax = first_argument_tokens.consume_a_token().token().ident();
        } else {
            is_dimension_unit = syntax_ident == "%"sv
                || Angle::unit_from_name(syntax_ident).has_value()
                || Flex::unit_from_name(syntax_ident).has_value()
                || Frequency::unit_from_name(syntax_ident).has_value()
                || Length::unit_from_name(syntax_ident).has_value()
                || Resolution::unit_from_name(syntax_ident).has_value()
                || Time::unit_from_name(syntax_ident).has_value();
            if (is_dimension_unit)
                maybe_syntax = first_argument_tokens.consume_a_token().token().ident();
        }
    }
    first_argument_tokens.discard_whitespace();
    if (first_argument_tokens.has_next_token())
        return failure();

    // 3. If attr name exists as an attribute on el, let attr value be its value; otherwise jump to the last step (labeled FAILURE).
    // FIXME: Attribute namespaces
    auto attribute_value = element.element().get_attribute(attribute_name);
    if (!attribute_value.has_value())
        return failure();

    // 4. If syntax is null or the keyword raw-string, return a CSS <string> whose value is attr value.
    // NOTE: No parsing or modification of any kind is performed on the value.
    if (!maybe_syntax.has_value() || maybe_syntax->equals_ignoring_ascii_case("raw-string"sv))
        return { Token::create_string(*attribute_value) };
    auto syntax = maybe_syntax.release_value();

    // 5. Substitute arbitrary substitution functions in attr value, with «"attribute", attr name» as the substitution
    //    context, then parse with a attr value, with syntax and el. If that succeeds, return the result; otherwise,
    //    jump to the last step (labeled FAILURE).
    auto parser = Parser::create(ParsingParams { element.element().document() }, attribute_value.value());
    auto unsubstituted_values = parser.parse_as_list_of_component_values();
    auto substituted_values = substitute_arbitrary_substitution_functions(element, guarded_contexts, unsubstituted_values, SubstitutionContext { SubstitutionContext::DependencyType::Attribute, attribute_name.to_string() });

    // FIXME: Parse using the syntax. For now we just handle `<attr-unit>` here.
    TokenStream value_tokens { substituted_values };
    value_tokens.discard_whitespace();
    auto const& component_value = value_tokens.consume_a_token();
    value_tokens.discard_whitespace();
    if (value_tokens.has_next_token())
        return failure();

    if (component_value.is(Token::Type::Number) && is_dimension_unit)
        return { Token::create_dimension(component_value.token().number_value(), syntax) };

    return failure();

    // 6. FAILURE:
    // NB: Step 6 is a lambda defined at the top of the function.
}

// https://drafts.csswg.org/css-variables-1/#replace-a-var-function
static Vector<ComponentValue> replace_a_var_function(DOM::AbstractElement& element, GuardedSubstitutionContexts& guarded_contexts, ArbitrarySubstitutionFunctionArguments const& arguments)
{
    // 1. Let el be the element that the style containing the var() function is being applied to.
    //    Let first arg be the first <declaration-value> in arguments.
    //    Let second arg be the <declaration-value>? passed after the comma, or null if there was no comma.
    auto const& first_argument = arguments.first();
    auto const second_argument = arguments.get(1);

    // 2. Substitute arbitrary substitution functions in first arg, then parse it as a <custom-property-name>.
    //    If parsing returned a <custom-property-name>, let result be the computed value of the corresponding custom
    //    property on el. Otherwise, let result be the guaranteed-invalid value.
    auto substituted_first_argument = substitute_arbitrary_substitution_functions(element, guarded_contexts, first_argument);
    TokenStream name_tokens { substituted_first_argument };
    name_tokens.discard_whitespace();
    auto& name_token = name_tokens.consume_a_token();
    name_tokens.discard_whitespace();

    Vector<ComponentValue> result;
    if (name_tokens.has_next_token() || !name_token.is(Token::Type::Ident) || !is_a_custom_property_name_string(name_token.token().ident())) {
        result = { ComponentValue { GuaranteedInvalidValue {} } };
    } else {
        // Look up the value of the custom property
        auto& custom_property_name = name_token.token().ident();
        auto custom_property_value = StyleComputer::compute_value_of_custom_property(element, custom_property_name, guarded_contexts);
        if (custom_property_value->is_guaranteed_invalid()) {
            result = { ComponentValue { GuaranteedInvalidValue {} } };
        } else if (custom_property_value->is_unresolved()) {
            result = custom_property_value->as_unresolved().values();
        } else {
            dbgln_if(CSS_PARSER_DEBUG, "Custom property `{}` is an unsupported type: {}", custom_property_name, to_underlying(custom_property_value->type()));
            result = { ComponentValue { GuaranteedInvalidValue {} } };
        }
    }

    // FIXME: 3. If the custom property named by the var()’s first argument is animation-tainted, and the var() is being used
    //    in a property that is not animatable, set result to the guaranteed-invalid value.

    // 4. If result contains the guaranteed-invalid value, and second arg was provided, set result to the result of substitute arbitrary substitution functions on second arg.
    if (contains_guaranteed_invalid_value(result) && second_argument.has_value())
        result = substitute_arbitrary_substitution_functions(element, guarded_contexts, second_argument.value());

    // 5. Return result.
    return result;
}

static ErrorOr<void> substitute_arbitrary_substitution_functions_step_2(DOM::AbstractElement& element, GuardedSubstitutionContexts& guarded_contexts, TokenStream<ComponentValue>& source, Vector<ComponentValue>& dest)
{
    // Step 2 of https://drafts.csswg.org/css-values-5/#substitute-arbitrary-substitution-function
    // 2. For each arbitrary substitution function func in values (ordered via a depth-first pre-order traversal) that
    //    is not nested in the contents of another arbitrary substitution function:
    while (source.has_next_token()) {
        auto const& value = source.consume_a_token();
        if (value.is_function()) {
            auto const& source_function = value.function();
            if (auto maybe_function_id = to_arbitrary_substitution_function(source_function.name); maybe_function_id.has_value()) {
                auto function_id = maybe_function_id.release_value();

                // FIXME: 1. Substitute early-invoked functions in func’s contents, and let early result be the result.
                auto const& early_result = source_function.value;

                // 2. If early result contains the guaranteed-invalid value, replace func in values with the guaranteed-invalid
                //    value and continue.
                if (contains_guaranteed_invalid_value(early_result)) {
                    dest.empend(GuaranteedInvalidValue {});
                    continue;
                }

                // 3. Parse early result according to func’s argument grammar. If this returns failure, replace func in values
                //    with the guaranteed-invalid value and continue; otherwise, let arguments be the result.
                auto maybe_arguments = parse_according_to_argument_grammar(function_id, early_result);
                if (!maybe_arguments.has_value()) {
                    dest.empend(GuaranteedInvalidValue {});
                    continue;
                }
                auto arguments = maybe_arguments.release_value();

                // 4. Replace an arbitrary substitution function for func, given arguments, as defined by that function.
                //    Let result be the returned list of component values.
                auto result = replace_an_arbitrary_substitution_function(element, guarded_contexts, function_id, arguments);

                // 5. If result contains the guaranteed-invalid value, replace func in values with the guaranteed-invalid value.
                //    Otherwise, replace func in values with result.
                if (contains_guaranteed_invalid_value(result)) {
                    dest.empend(GuaranteedInvalidValue {});
                } else {
                    // NB: Because we're doing this in one pass recursively, we now need to substitute any ASFs in result.
                    TokenStream result_stream { result };
                    Vector<ComponentValue> result_after_processing;
                    TRY(substitute_arbitrary_substitution_functions_step_2(element, guarded_contexts, result_stream, result_after_processing));

                    // NB: Protect against the billion-laughs attack by limiting to an arbitrary large number of tokens.
                    // https://drafts.csswg.org/css-values-5/#long-substitution
                    if (source.remaining_token_count() + result_after_processing.size() > 16384) {
                        dest.clear();
                        dest.empend(GuaranteedInvalidValue {});
                        return Error::from_string_literal("Stopped expanding arbitrary substitution functions: maximum length reached.");
                    }

                    dest.extend(result_after_processing);
                }
                continue;
            }

            Vector<ComponentValue> function_values;
            TokenStream source_function_contents { source_function.value };
            TRY(substitute_arbitrary_substitution_functions_step_2(element, guarded_contexts, source_function_contents, function_values));
            dest.empend(Function { source_function.name, move(function_values) });
            continue;
        }
        if (value.is_block()) {
            auto const& source_block = value.block();
            TokenStream source_block_values { source_block.value };
            Vector<ComponentValue> block_values;
            TRY(substitute_arbitrary_substitution_functions_step_2(element, guarded_contexts, source_block_values, block_values));
            dest.empend(SimpleBlock { source_block.token, move(block_values) });
            continue;
        }
        dest.empend(value);
    }

    return {};
}

// https://drafts.csswg.org/css-values-5/#substitute-arbitrary-substitution-function
Vector<ComponentValue> substitute_arbitrary_substitution_functions(DOM::AbstractElement& element, GuardedSubstitutionContexts& guarded_contexts, Vector<ComponentValue> const& values, Optional<SubstitutionContext> context)
{
    // To substitute arbitrary substitution functions in a sequence of component values values, given an optional
    // substitution context context:

    // 1. Guard context for the remainder of this algorithm. If context is marked as a cyclic substitution context,
    //    return the guaranteed-invalid value.
    if (context.has_value()) {
        guarded_contexts.guard(context.value());
        if (context->is_cyclic)
            return { ComponentValue { GuaranteedInvalidValue {} } };
    }
    ScopeGuard const guard { [&] {
        if (context.has_value())
            guarded_contexts.unguard(context.value());
    } };

    // 2. For each arbitrary substitution function func in values (ordered via a depth-first pre-order traversal) that
    //    is not nested in the contents of another arbitrary substitution function:
    Vector<ComponentValue> new_values;
    TokenStream source { values };
    auto maybe_error = substitute_arbitrary_substitution_functions_step_2(element, guarded_contexts, source, new_values);
    if (maybe_error.is_error()) {
        dbgln_if(CSS_PARSER_DEBUG, "{} (context? {})", maybe_error.release_error(), context.map([](auto& it) { return it.to_string(); }));
        return { ComponentValue { GuaranteedInvalidValue {} } };
    }

    // 3. If context is marked as a cyclic substitution context, return the guaranteed-invalid value.
    // NOTE: Nested arbitrary substitution functions may have marked context as cyclic in step 2.
    if (context.has_value() && context->is_cyclic)
        return { ComponentValue { GuaranteedInvalidValue {} } };

    // 4. Return values.
    return new_values;
}

Optional<ArbitrarySubstitutionFunctionArguments> parse_according_to_argument_grammar(ArbitrarySubstitutionFunction function, Vector<ComponentValue> const& values)
{
    // Equivalent to `<declaration-value> , <declaration-value>?`, used by multiple argument grammars.
    auto parse_declaration_value_then_optional_declaration_value = [](Vector<ComponentValue> const& values) -> Optional<ArbitrarySubstitutionFunctionArguments> {
        TokenStream tokens { values };

        auto first_argument = Parser::parse_declaration_value(tokens, Parser::StopAtComma::Yes);
        if (!first_argument.has_value())
            return OptionalNone {};

        if (!tokens.has_next_token())
            return ArbitrarySubstitutionFunctionArguments { first_argument.release_value() };

        if (!tokens.next_token().is(Token::Type::Comma))
            return {};

        tokens.discard_a_token(); // ,

        auto second_argument = Parser::parse_declaration_value(tokens, Parser::StopAtComma::No);
        if (tokens.has_next_token())
            return OptionalNone {};
        return ArbitrarySubstitutionFunctionArguments { first_argument.release_value(), second_argument.value_or({}) };
    };

    switch (function) {
    case ArbitrarySubstitutionFunction::Attr:
        // https://drafts.csswg.org/css-values-5/#attr-notation
        // <attr-args> = attr( <declaration-value> , <declaration-value>? )
        return parse_declaration_value_then_optional_declaration_value(values);
    case ArbitrarySubstitutionFunction::Var:
        // https://drafts.csswg.org/css-variables/#funcdef-var
        // <var-args> = var( <declaration-value> , <declaration-value>? )
        return parse_declaration_value_then_optional_declaration_value(values);
    }
    VERIFY_NOT_REACHED();
}

// https://drafts.csswg.org/css-values-5/#replace-an-arbitrary-substitution-function
Vector<ComponentValue> replace_an_arbitrary_substitution_function(DOM::AbstractElement& element, GuardedSubstitutionContexts& guarded_contexts, ArbitrarySubstitutionFunction function, ArbitrarySubstitutionFunctionArguments const& arguments)
{
    switch (function) {
    case ArbitrarySubstitutionFunction::Attr:
        return replace_an_attr_function(element, guarded_contexts, arguments);
    case ArbitrarySubstitutionFunction::Var:
        return replace_a_var_function(element, guarded_contexts, arguments);
    }
    VERIFY_NOT_REACHED();
}

}
