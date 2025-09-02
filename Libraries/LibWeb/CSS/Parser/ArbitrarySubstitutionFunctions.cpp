/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/ArbitrarySubstitutionFunctions.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/Document.h>
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
    if (name.equals_ignoring_ascii_case("env"sv))
        return ArbitrarySubstitutionFunction::Env;
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

    struct RawString { };
    Variant<Empty, NonnullOwnPtr<SyntaxNode>, RawString> syntax;
    Optional<FlyString> unit_name;

    auto failure = [&] -> Vector<ComponentValue> {
        // This is step 6, but defined here for convenience.

        // 1. If second arg is null, and syntax was omitted, return an empty CSS <string>.
        if (!second_argument.has_value() && syntax.has<Empty>())
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
    if (first_argument_tokens.next_token().is(Token::Type::Ident)) {
        auto const& syntax_ident = first_argument_tokens.next_token().token().ident();
        if (syntax_ident.equals_ignoring_ascii_case("raw-string"sv)) {
            first_argument_tokens.discard_a_token(); // raw-string
            syntax = RawString {};
        } else if (syntax_ident == "%"sv
            || dimension_for_unit(syntax_ident).has_value()) {
            syntax = TypeSyntaxNode::create("number"_fly_string).release_nonnull<SyntaxNode>();
            unit_name = first_argument_tokens.consume_a_token().token().ident();
        } else {
            return failure();
        }
    } else if (first_argument_tokens.next_token().is_function("type"sv)) {
        auto const& type_function = first_argument_tokens.consume_a_token().function();
        if (auto parsed_syntax = parse_as_syntax(type_function.value)) {
            syntax = parsed_syntax.release_nonnull();
        } else {
            return failure();
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
    if (syntax.visit(
            [](Empty) { return true; },
            [](RawString) { return true; },
            [](auto&) { return false; })) {
        return { Token::create_string(*attribute_value) };
    }

    // 5. Substitute arbitrary substitution functions in attr value, with «"attribute", attr name» as the substitution
    //    context, then parse with a <syntax> attr value, with syntax and el. If that succeeds, return the result;
    //    otherwise, jump to the last step (labeled FAILURE).
    auto parser = Parser::create(ParsingParams { element.element().document() }, attribute_value.value());
    auto unsubstituted_values = parser.parse_as_list_of_component_values();
    auto substituted_values = substitute_arbitrary_substitution_functions(element, guarded_contexts, unsubstituted_values,
        SubstitutionContext { SubstitutionContext::DependencyType::Attribute, attribute_name.to_string() });

    auto parsed_value = parse_with_a_syntax(ParsingParams { element.document() }, substituted_values, *syntax.get<NonnullOwnPtr<SyntaxNode>>(), element);
    if (parsed_value->is_guaranteed_invalid())
        return failure();

    if (unit_name.has_value()) {
        // https://drafts.csswg.org/css-values-5/#ref-for-typedef-attr-type%E2%91%A0
        // If given as an <attr-unit> value, the value is first parsed as if type(<number>) was specified, then the
        // resulting numeric value is turned into a dimension with the corresponding unit, or a percentage if % was
        // given. Values that fail to parse as a <number> trigger fallback.

        // FIXME: The spec is ambiguous about what we should do for non-number-literals.
        //        Chromium treats them as invalid, so copy that for now.
        //        Spec issue: https://github.com/w3c/csswg-drafts/issues/12479
        if (!parsed_value->is_number())
            return failure();
        return { Token::create_dimension(parsed_value->as_number().number(), unit_name.release_value()) };
    }

    return parsed_value->tokenize();

    // 6. FAILURE:
    // NB: Step 6 is a lambda defined at the top of the function.
}

// https://drafts.csswg.org/css-env/#substitute-an-env
static Vector<ComponentValue> replace_an_env_function(DOM::AbstractElement& element, GuardedSubstitutionContexts& guarded_contexts, ArbitrarySubstitutionFunctionArguments const& arguments)
{
    // AD-HOC: env() is not defined as an ASF (and was defined before the ASF concept was), but behaves a lot like one.
    // So, this is a combination of the spec's "substitute an env()" algorithm linked above, and the "replace a FOO function()" algorithms.

    auto const& first_argument = arguments.first();
    auto const second_argument = arguments.get(1);

    // AD-HOC: Substitute ASFs in the first argument.
    auto substituted_first_argument = substitute_arbitrary_substitution_functions(element, guarded_contexts, first_argument);

    // AD-HOC: Parse the arguments.
    // env() = env( <custom-ident> <integer [0,∞]>*, <declaration-value>? )
    TokenStream first_argument_tokens { substituted_first_argument };
    first_argument_tokens.discard_whitespace();
    auto& name_token = first_argument_tokens.consume_a_token();
    if (!name_token.is(Token::Type::Ident))
        return { ComponentValue { GuaranteedInvalidValue {} } };
    auto& name = name_token.token().ident();
    first_argument_tokens.discard_whitespace();

    Vector<i64> indices;
    // FIXME: Are non-literal <integer>s allowed here?
    while (first_argument_tokens.has_next_token()) {
        auto& maybe_integer = first_argument_tokens.consume_a_token();
        if (!maybe_integer.is(Token::Type::Number))
            return { ComponentValue { GuaranteedInvalidValue {} } };
        auto& number = maybe_integer.token().number();
        if (number.is_integer() && number.integer_value() >= 0)
            indices.append(number.integer_value());
        else
            return { ComponentValue { GuaranteedInvalidValue {} } };
        first_argument_tokens.discard_whitespace();
    }

    // 1. If the name provided by the first argument of the env() function is a recognized environment variable name,
    //    the number of supplied integers matches the number of dimensions of the environment variable referenced by
    //    that name, and values of the indices correspond to a known sub-value, replace the env() function by the value
    //    of the named environment variable.
    if (auto environment_variable = environment_variable_from_string(name);
        environment_variable.has_value() && indices.size() == environment_variable_dimension_count(*environment_variable)) {

        auto result = element.document().environment_variable_value(*environment_variable, indices);
        if (result.has_value())
            return result.release_value();
    }

    // 2. Otherwise, if the env() function has a fallback value as its second argument, replace the env() function by
    //    the fallback value. If there are any env() references in the fallback, substitute them as well.
    // AD-HOC: Substitute all ASFs in the result.
    if (second_argument.has_value())
        return substitute_arbitrary_substitution_functions(element, guarded_contexts, second_argument.value());

    // 3. Otherwise, the property or descriptor containing the env() function is invalid at computed-value time.
    return { ComponentValue { GuaranteedInvalidValue {} } };
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
    if (maybe_error.is_error())
        return { ComponentValue { GuaranteedInvalidValue {} } };

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
    case ArbitrarySubstitutionFunction::Env:
        // https://drafts.csswg.org/css-env/#env-function
        // AD-HOC: This doesn't have an argument-grammar definition.
        //         However, it follows the same format of "some CVs, then an optional comma and a fallback".
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
    case ArbitrarySubstitutionFunction::Env:
        return replace_an_env_function(element, guarded_contexts, arguments);
    case ArbitrarySubstitutionFunction::Var:
        return replace_a_var_function(element, guarded_contexts, arguments);
    }
    VERIFY_NOT_REACHED();
}

}
