/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSFunctionRule.h>
#include <LibWeb/CSS/CSSUnparsedValue.h>
#include <LibWeb/CSS/CSSVariableReferenceValue.h>
#include <LibWeb/CSS/HypotheticalElement.h>
#include <LibWeb/CSS/Parser/ArbitrarySubstitutionFunctions.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustQueryParsing.h>
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS::Parser {

Vector<ComponentValue> unresolved_style_value_components(UnresolvedStyleValue const& value)
{
    auto parser = Parser::create(ParsingParams {}, value.token_source());
    auto components = parser.parse_as_list_of_component_values();
    if (value.contains_attr_tainted_values()) {
        for (auto& component : components)
            component.set_attr_tainted();
    }
    return components;
}

Utf16String serialize_style_value_for_tokenization(StyleValue const& value)
{
    if (value.is_unresolved())
        return value.as_unresolved().token_source();
    return serialize_a_series_of_component_values_for_retokenization(value.tokenize());
}

Utf16String serialize_style_value_components(StyleValue const& value)
{
    if (value.is_unresolved())
        return value.as_unresolved().serialized_components();
    return serialize_a_series_of_component_values(value.tokenize());
}

static GC::Ref<CSSUnparsedValue> reify_a_list_of_component_values(ReadonlySpan<ComponentValue>);

// https://drafts.css-houdini.org/css-typed-om-1/#reify-var
static GC::Ptr<CSSVariableReferenceValue> reify_a_var_reference(Function function)
{
    // NB: A var() might not be representable as a CSSVariableReferenceValue, for example if it has invalid syntax or
    //    it contains an ASF in its variable-name slot. In those cases, we return null here, so it's treated like a
    //    regular function.
    auto maybe_var_arguments = parse_according_to_argument_grammar(ArbitrarySubstitutionFunction::Var, function.value);
    if (!maybe_var_arguments.has_value())
        return nullptr;
    auto var_arguments = maybe_var_arguments.release_value().get<DeclarationValueList>();

    TokenStream tokens { var_arguments.first() };
    tokens.discard_whitespace();
    auto& maybe_variable = tokens.consume_a_token();
    tokens.discard_whitespace();
    if (tokens.has_next_token()
        || !maybe_variable.is(Token::Type::Ident)
        || !is_a_custom_property_name_string(maybe_variable.token().ident()))
        return nullptr;

    auto variable = maybe_variable.token().ident();
    GC::Ptr<CSSUnparsedValue> fallback;
    if (var_arguments.size() > 1)
        fallback = reify_a_list_of_component_values(var_arguments[1]);
    return CSSVariableReferenceValue::create(move(variable), move(fallback));
}

class UnresolvedValueReifier {
public:
    static Vector<CSSUnparsedSegment> reify(ReadonlySpan<ComponentValue> source_values)
    {
        UnresolvedValueReifier reifier;
        reifier.process_values(source_values);
        if (!reifier.m_unserialized_values.is_empty())
            reifier.serialize_unserialized_values();
        return move(reifier.m_reified_values);
    }

private:
    void process_values(ReadonlySpan<ComponentValue> source_values)
    {
        // NB: var() could be arbitrarily nested within other functions and blocks, so we have to walk the tree.
        //     Also, a var() might not be representable, if it has an ASF in place of its name, so those will be part
        //     of a string instead.
        for (auto const& component_value : source_values) {
            if (component_value.is_function("var"_utf16)) {
                if (auto var_reference = reify_a_var_reference(component_value.function())) {
                    serialize_unserialized_values();
                    m_reified_values.append(GC::Ref { *var_reference });
                    continue;
                }
            }

            if (component_value.is_function()) {
                auto& function = component_value.function();
                m_unserialized_values.append(Token::create_function(function.name, function.name_token.original_source_text()));
                process_values(function.value);
                m_unserialized_values.append(Token::create(function.end_token.type(), function.end_token.original_source_text()));
                continue;
            }

            if (component_value.is_block()) {
                auto& block = component_value.block();
                m_unserialized_values.append(Token::create(block.token.type(), block.token.original_source_text()));
                process_values(block.value);
                m_unserialized_values.append(Token::create(block.end_token.type(), block.end_token.original_source_text()));
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
    Vector<ComponentValue> m_unserialized_values {};
};

static GC::Ref<CSSUnparsedValue> reify_a_list_of_component_values(ReadonlySpan<ComponentValue> component_values)
{
    auto reified_values = UnresolvedValueReifier::reify(component_values);
    return CSSUnparsedValue::create(move(reified_values));
}

GC::Ref<CSSStyleValue> reify_unresolved_style_value(UnresolvedStyleValue const& value)
{
    auto component_values = unresolved_style_value_components(value);
    return reify_a_list_of_component_values(component_values);
}

PropertySubstitutionContextDependency PropertySubstitutionContextDependency::create(Utf16String property_name, AbstractOrHypotheticalElement const& element)
{
    // https://drafts.csswg.org/css-mixins/#resolve-function-styles
    // For a given custom property prop, during property replacement for that property, the substitution context also
    // includes custom function. In other words, the substitution context is «"property", prop’s name, custom function»

    // NB: If we are are resolving a hypothetical element we know that we are evaluating a custom function
    if (element.has<HypotheticalElement*>())
        return PropertySubstitutionContextDependency { move(property_name), element.get<HypotheticalElement*>()->custom_function };

    return PropertySubstitutionContextDependency { move(property_name), nullptr };
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

bool GuardedSubstitutionContexts::mark_existing_as_cyclic(SubstitutionContext const& context)
{
    for (auto& existing_context : m_contexts) {
        if (existing_context == context) {
            existing_context.is_cyclic = true;
            return true;
        }
    }
    return false;
}

Optional<ArbitrarySubstitutionFunction> to_arbitrary_substitution_function(Utf16View name)
{
    if (name.equals_ignoring_ascii_case("attr"sv))
        return ArbitrarySubstitutionFunction::Attr;
    if (name.starts_with("--"sv))
        return ArbitrarySubstitutionFunction::DashedFunction;
    if (name.equals_ignoring_ascii_case("env"sv))
        return ArbitrarySubstitutionFunction::Env;
    if (name.equals_ignoring_ascii_case("if"sv))
        return ArbitrarySubstitutionFunction::If;
    if (name.equals_ignoring_ascii_case("inherit"sv))
        return ArbitrarySubstitutionFunction::Inherit;
    if (name.equals_ignoring_ascii_case("var"sv))
        return ArbitrarySubstitutionFunction::Var;
    return {};
}

bool contains_guaranteed_invalid_value(ReadonlySpan<ComponentValue> values)
{
    for (auto const& value : values) {
        if (value.contains_guaranteed_invalid_value())
            return true;
    }
    return false;
}

bool contains_attr_tainted_value(ReadonlySpan<ComponentValue> values)
{
    for (auto const& value : values) {
        if (value.contains_attr_tainted_value())
            return true;
    }
    return false;
}

static Vector<ComponentValue> mark_as_attr_tainted(Vector<ComponentValue> values)
{
    for (auto& value : values)
        value.set_attr_tainted();
    return values;
}

// https://drafts.csswg.org/css-values-5/#replace-an-attr-function
static Vector<ComponentValue> replace_an_attr_function(AbstractOrHypotheticalElement& element, GuardedSubstitutionContexts& guarded_contexts, ArbitrarySubstitutionReplacementContext const& replacement_context, ArbitrarySubstitutionFunctionArguments const& arguments)
{
    // 1. Let el be the element that the style containing the attr() function is being applied to.
    //    Let first arg be the first <declaration-value> in arguments.
    //    Let second arg be the <declaration-value>? passed after the comma, or null if there was no comma.
    auto const& declaration_value_list = arguments.get<DeclarationValueList>();

    auto const& first_argument = declaration_value_list.first();
    auto const second_argument = declaration_value_list.get(1);

    Utf16FlyString attribute_name;

    struct RawStringKeyword { };
    struct NumberKeyword { };
    struct AttrUnit {
        Utf16FlyString name;
    };
    Variant<Empty, NonnullRefPtr<SyntaxNode>, RawStringKeyword, NumberKeyword, AttrUnit> syntax;

    auto failure = [&] -> Vector<ComponentValue> {
        // This is step 7, but defined here for convenience.

        // 1. If second arg is null, and syntax was omitted, return an empty CSS <string>.
        if (!second_argument.has_value() && syntax.has<Empty>())
            return { Token::create_string({}) };

        // 2. If second arg is null, return the guaranteed-invalid value.
        if (!second_argument.has_value())
            return { ComponentValue { GuaranteedInvalidValue {} } };

        // 3. Substitute arbitrary substitution functions in second arg, and return the result.
        return substitute_arbitrary_substitution_functions(element, guarded_contexts, replacement_context, second_argument.value());
    };

    // 2. Substitute arbitrary substitution functions in first arg, then parse it as <attr-name> <attr-type>?.
    //    If that returns failure, jump to the last step (labeled FAILURE).
    //    Otherwise, let attr name and syntax be the results of parsing (with syntax being null if <attr-type> was
    //    omitted), processed as specified in the definition of those arguments.
    auto substituted = substitute_arbitrary_substitution_functions(element, guarded_contexts, replacement_context, first_argument);
    TokenStream first_argument_tokens { substituted };
    // <attr-name> = [ <ident-token>? '|' ]? <ident-token>
    // FIXME: Support optional attribute namespace
    if (!first_argument_tokens.next_token().is(Token::Type::Ident))
        return failure();
    attribute_name = first_argument_tokens.consume_a_token().token().ident();
    first_argument_tokens.discard_whitespace();

    // <attr-type> = type( <syntax> ) | raw-string | number | <attr-unit>
    if (first_argument_tokens.next_token().is(Token::Type::Ident)) {
        auto const& syntax_ident = first_argument_tokens.next_token().token().ident();
        if (syntax_ident.equals_ignoring_ascii_case("raw-string"sv)) {
            first_argument_tokens.discard_a_token(); // raw-string
            syntax = RawStringKeyword {};
        } else if (syntax_ident.equals_ignoring_ascii_case("number"sv)) {
            first_argument_tokens.discard_a_token(); // number
            syntax = NumberKeyword {};
        } else if (dimension_for_unit(syntax_ident.view()).has_value()) {
            auto unit = first_argument_tokens.consume_a_token().token().ident();
            syntax = AttrUnit { unit };
        } else {
            return failure();
        }
    } else if (first_argument_tokens.next_token().is_delim('%')) {
        first_argument_tokens.discard_a_token(); // %
        syntax = AttrUnit { "%"_utf16_fly_string };
    } else if (first_argument_tokens.next_token().is_function("type"_utf16)) {
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
    auto attribute_value = element.abstract_element().element().get_attribute(attribute_name);
    if (!attribute_value.has_value())
        return failure();

    // 4. If syntax is the keyword number or an <attr-unit> value, parse attr value against <attr-type>.
    //    If that succeeds, return the result; otherwise, jump to the last step (labeled FAILURE).
    // NOTE: No parsing or modification of any kind is performed on the value.
    auto parse_as_number = [&]() -> RefPtr<NumberStyleValue const> {
        auto parser = Parser::create(ParsingParams { element.document() }, attribute_value->utf16_view());
        auto unsubstituted_values = parser.parse_as_list_of_component_values();
        auto syntax_node = TypeSyntaxNode::create("number"_utf16_fly_string);
        auto parsed_value = parse_with_a_syntax(ParsingParams { element.document() }, unsubstituted_values, *syntax_node);
        if (parsed_value->is_guaranteed_invalid())
            return {};

        // FIXME: The spec is ambiguous about what we should do for non-number-literals.
        //        Chromium treats them as invalid, so copy that for now.
        //        Spec issue: https://github.com/w3c/csswg-drafts/issues/12479
        if (!parsed_value->is_number())
            return {};

        return parsed_value->as_number();
    };

    bool return_from_step_4 = false;
    auto step_4_result = syntax.visit(
        // https://drafts.csswg.org/css-values-5/#ref-for-typedef-attr-type%E2%91%A0
        [&](NumberKeyword) -> Optional<Vector<ComponentValue>> {
            // If given as the number keyword, it causes the attribute’s literal value, after stripping leading and
            // trailing whitespace, to be parsed as a <number-token>. Values that fail to parse trigger fallback.
            return_from_step_4 = true;
            auto parsed_number = parse_as_number();
            if (!parsed_number)
                return {};
            return Vector<ComponentValue> { Token::create_number(Number { Number::Type::Number, parsed_number->number() }) };
        },
        [&](AttrUnit const& attr_unit) -> Optional<Vector<ComponentValue>> {
            // If given as an <attr-unit> value, the value is first parsed as if number keyword was specified, then the
            // resulting numeric value is turned into a dimension with the corresponding unit, or a percentage if % was
            // given. Same as for number <attr-type>, values that do not correspond to the <number-token> production
            // trigger fallback.
            return_from_step_4 = true;
            auto parsed_number = parse_as_number();
            if (!parsed_number)
                return {};
            if (attr_unit.name == "%"_utf16_fly_string)
                return Vector<ComponentValue> { Token::create_percentage(Number { Number::Type::Number, parsed_number->number() }) };
            return Vector<ComponentValue> { Token::create_dimension(parsed_number->number(), attr_unit.name) };
        },
        [&](auto&) -> Optional<Vector<ComponentValue>> {
            return OptionalNone {};
        });
    if (return_from_step_4) {
        if (step_4_result.has_value())
            return mark_as_attr_tainted(step_4_result.release_value());
        return failure();
    }

    // 5. If syntax is null or the keyword raw-string, return a CSS <string> whose value is attr value.
    // NOTE: No parsing or modification of any kind is performed on the value.
    if (syntax.visit(
            [](Empty) { return true; },
            [](RawStringKeyword) { return true; },
            [](auto&) { return false; })) {
        return mark_as_attr_tainted({ Token::create_string(Utf16FlyString::from_utf16(attribute_value->utf16_view())) });
    }

    // 6. Substitute arbitrary substitution functions in attr value, with «"attribute", attr name» as the substitution
    //    context, then parse with a <syntax> attr value, with syntax and el. If that succeeds, return the result;
    //    otherwise, jump to the last step (labeled FAILURE).
    auto parser = Parser::create(ParsingParams { element.document() }, attribute_value->utf16_view());
    auto unsubstituted_values = parser.parse_as_list_of_component_values();
    auto substituted_values = substitute_arbitrary_substitution_functions(element, guarded_contexts, replacement_context, unsubstituted_values,
        SubstitutionContext { AttributeSubstitutionContextDependency { attribute_name.to_utf16_string() } });

    if (contains_guaranteed_invalid_value(substituted_values))
        return failure();

    auto parsed_value = parse_with_a_syntax(ParsingParams { element.document() }, substituted_values, *syntax.get<NonnullRefPtr<SyntaxNode>>());
    if (parsed_value->is_guaranteed_invalid())
        return failure();
    return mark_as_attr_tainted(parsed_value->tokenize());

    // 7. FAILURE:
    // NB: Step 7 is a lambda defined at the top of the function.
}

// https://drafts.csswg.org/css-mixins/#replace-a-dashed-function
static Vector<ComponentValue> replace_a_dashed_function(AbstractOrHypotheticalElement& element, GuardedSubstitutionContexts& guarded_contexts, ArbitrarySubstitutionReplacementContext const& replacement_context, Utf16FlyString const& function_name, ArbitrarySubstitutionFunctionArguments const& arguments)
{
    auto const& declaration_value_list = arguments.get<DeclarationValueList>();

    // Calling a dashed function is what makes an element a consumer of one, and that is true whether
    // or not the name resolves. An element whose call found no definition is exactly the one an
    // `@function` rule arriving later has to reach, so it has to be in the index that finds it before
    // the lookup can send it away. A hypothetical element is not in the tree and has no style to
    // invalidate; a real one is what an `@function` change has to reach.
    if (element.has<DOM::AbstractElement>())
        element.get<DOM::AbstractElement>().element().set_style_uses_custom_function();

    // 1. Let function be the result of dereferencing the dashed function’s name as a tree-scoped reference. If no such
    //    name exists, return the guaranteed-invalid value.

    // NB: For nested function calls we use the style scope in which the function was defined, not the style scope of
    //     the real element.
    // FIXME: For top-level calls we should use the style scope of the relevant CSS rule rather than the element's style
    //        scope - see failing tests in function-shadow.html
    auto const& function_and_scope = element.style_scope().get_function_definition(function_name);

    if (!function_and_scope.has_value())
        return { ComponentValue { GuaranteedInvalidValue {} } };

    // 2. For each arg in arguments, substitute arbitrary substitution functions in arg, and replace arg with the
    //    result.
    // Note: This may leave some (or all) arguments as the guaranteed-invalid value, triggering default values (if any).
    Vector<Vector<ComponentValue>> substituted_arguments;
    substituted_arguments.ensure_capacity(declaration_value_list.size());

    for (auto const& arg : declaration_value_list)
        substituted_arguments.unchecked_append(substitute_arbitrary_substitution_functions(element, guarded_contexts, replacement_context, arg));

    // 3. If dashed function is being substituted into a property on an element, let calling context be a calling
    //    context with that element and that property.
    //
    //    Otherwise, it’s being substituted into a descriptor on a "hypothetical element", while evaluating another
    //    custom function. Let calling context be a calling context with that "hypothetical element" and that
    //    descriptor.

    Optional<Utf16View> property_or_descriptor_name;

    auto context_stack = guarded_contexts.as_readonly_span();

    for (size_t i = context_stack.size(); i-- > 0;) {
        if (auto const* property_dependency = context_stack[i]->dependency.get_pointer<PropertySubstitutionContextDependency>()) {
            property_or_descriptor_name = property_dependency->property_name;
            break;
        }
    }

    // NB: The root context is always a property context so we should always find a property/descriptor name.
    VERIFY(property_or_descriptor_name.has_value());

    CSSFunctionRule::CallingContext calling_context {
        .element = element,
        .property_or_descriptor_name = property_or_descriptor_name.release_value(),
        .computed_style_for_custom_property_resolution = replacement_context.computed_style_for_custom_property_resolution,
        .style_scope = function_and_scope->scope
    };

    // 4. Evaluate a custom function, using function, arguments, and calling context, and return the equivalent token
    //    sequence of the value resulting from the evaluation.
    return function_and_scope->function->evaluate_a_custom_function(guarded_contexts, substituted_arguments, calling_context)->tokenize();
}

// https://drafts.csswg.org/css-env/#substitute-an-env
static Vector<ComponentValue> replace_an_env_function(AbstractOrHypotheticalElement& element, GuardedSubstitutionContexts& guarded_contexts, ArbitrarySubstitutionReplacementContext const& replacement_context, ArbitrarySubstitutionFunctionArguments const& arguments)
{
    // AD-HOC: env() is not defined as an ASF (and was defined before the ASF concept was), but behaves a lot like one.
    // So, this is a combination of the spec's "substitute an env()" algorithm linked above, and the "replace a FOO function()" algorithms.
    auto const& declaration_value_list = arguments.get<DeclarationValueList>();

    auto const& first_argument = declaration_value_list.first();
    auto const second_argument = declaration_value_list.get(1);

    // AD-HOC: Substitute ASFs in the first argument.
    auto substituted_first_argument = substitute_arbitrary_substitution_functions(element, guarded_contexts, replacement_context, first_argument);

    // AD-HOC: Parse the arguments.
    // env() = env( <custom-ident> <integer [0,∞]>*, <declaration-value>? )
    TokenStream first_argument_tokens { substituted_first_argument };
    first_argument_tokens.discard_whitespace();
    auto& name_token = first_argument_tokens.consume_a_token();
    if (!name_token.is(Token::Type::Ident))
        return { ComponentValue { GuaranteedInvalidValue {} } };
    auto& name = name_token.token().ident();
    first_argument_tokens.discard_whitespace();

    Vector<i32> indices;
    // FIXME: Are non-literal <integer>s allowed here?
    while (first_argument_tokens.has_next_token()) {
        auto& maybe_integer = first_argument_tokens.consume_a_token();
        if (!maybe_integer.is(Token::Type::Number))
            return { ComponentValue { GuaranteedInvalidValue {} } };
        if (maybe_integer.token().is_integer() && maybe_integer.token().to_integer() >= 0)
            indices.append(maybe_integer.token().to_integer());
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
        return substitute_arbitrary_substitution_functions(element, guarded_contexts, replacement_context, second_argument.value());

    // 3. Otherwise, the property or descriptor containing the env() function is invalid at computed-value time.
    return { ComponentValue { GuaranteedInvalidValue {} } };
}

// https://drafts.csswg.org/css-values-5/#replace-an-if-function
static Vector<ComponentValue> replace_an_if_function(AbstractOrHypotheticalElement& element, GuardedSubstitutionContexts& guarded_contexts, ArbitrarySubstitutionReplacementContext const& replacement_context, ArbitrarySubstitutionFunctionArguments const& arguments)
{
    // NB: We create a single parser and reuse that for parsing all the conditions
    auto parser = Parser::create(ParsingParams { element.document() }, ""sv);
    bool did_evaluate_attr_tainted_condition = false;

    // 1. For each <if-args-branch> branch in arguments:
    for (auto const& branch : arguments.get<IfArgs>()) {
        // 1. Substitute arbitrary substitution functions in the first <declaration-value> of branch, then parse the
        //    result as an <if-condition>. If parsing returns failure, continue; otherwise, let the result be condition.
        auto substituted_condition = substitute_arbitrary_substitution_functions(element, guarded_contexts, replacement_context, branch.condition);
        auto condition_is_attr_tainted = contains_attr_tainted_value(substituted_condition);

        TokenStream<ComponentValue> tokens { substituted_condition };
        auto maybe_parsed_if_condition = parser.parse_if_condition(tokens);

        if (!maybe_parsed_if_condition)
            continue;

        // 2. Evaluate condition.
        //    If a <style-query> in condition tests the value of a property, and guarding a substitution context
        //    «"property", referenced-property-name» would mark it as a cyclic substitution context, that query
        //    evaluates to false.
        //    If the result of condition is false, continue.
        bool did_evaluate_attr_tainted_style_query = false;
        auto condition_evaluation_result = maybe_parsed_if_condition->evaluate({
            .document = element.document(),
            .style_query_element = element,
            .guarded_contexts = guarded_contexts,
            .did_evaluate_attr_tainted_style_query = &did_evaluate_attr_tainted_style_query,
        });

        did_evaluate_attr_tainted_condition |= condition_is_attr_tainted || did_evaluate_attr_tainted_style_query;

        if (condition_evaluation_result == MatchResult::False)
            continue;

        // 3. Substitute arbitrary substitution functions in the second <declaration-value> of branch, and return the result.
        if (!branch.value.has_value())
            return {};

        auto result = substitute_arbitrary_substitution_functions(element, guarded_contexts, replacement_context, branch.value.value());
        if (did_evaluate_attr_tainted_condition)
            return mark_as_attr_tainted(move(result));
        return result;
    }

    // 2. Return nothing (an empty sequence of component values).
    return {};
}

// https://drafts.csswg.org/css-values-5/#replace-an-inherit-function
static Vector<ComponentValue> replace_an_inherit_function(AbstractOrHypotheticalElement& element, GuardedSubstitutionContexts& guarded_contexts, ArbitrarySubstitutionReplacementContext const& replacement_context, ArbitrarySubstitutionFunctionArguments const& arguments)
{
    // To replace an inherit() function, given a list of arguments:
    auto const& declaration_value_list = arguments.get<DeclarationValueList>();
    auto const& first_argument = declaration_value_list.first();
    auto const second_argument = declaration_value_list.get(1);

    // 1. Substitute arbitrary substitution functions in the first <declaration-value> of arguments, then parse it as a
    //    <custom-property-name>.
    auto substituted_first_argument = substitute_arbitrary_substitution_functions(element, guarded_contexts, replacement_context, first_argument);

    TokenStream first_argument_tokens { substituted_first_argument };
    first_argument_tokens.discard_whitespace();
    auto const& name_token = first_argument_tokens.consume_a_token();
    first_argument_tokens.discard_whitespace();

    // 2. If parsing returned a <custom-property-name>, and the inherited value of that custom property on the element
    //    does not contain the guaranteed-invalid value, return that inherited value.
    if (name_token.is(Token::Type::Ident) && is_a_custom_property_name_string(name_token.token().ident()) && first_argument_tokens.is_empty()) {
        auto const& custom_property_name = name_token.token().ident();
        element.abstract_element().element().record_style_custom_property_reference(custom_property_name);

        auto inherited_value = inherited_custom_property_value(
            element.get_registered_custom_property(custom_property_name),
            element,
            custom_property_name,
            replacement_context.computed_style_for_custom_property_resolution,
            guarded_contexts);

        auto inherited_value_tokens = inherited_value->tokenize();

        if (!contains_guaranteed_invalid_value(inherited_value_tokens))
            return inherited_value_tokens;
    }

    // 3. Otherwise, if a second <declaration-value>? was passed in arguments, substitute arbitrary substitution
    //    functions in that argument, and return the result.
    if (second_argument.has_value())
        return substitute_arbitrary_substitution_functions(element, guarded_contexts, replacement_context, second_argument.value());

    // 4. Otherwise, return the guaranteed-invalid value.
    return { ComponentValue { GuaranteedInvalidValue {} } };
}

// https://drafts.csswg.org/css-variables-1/#replace-a-var-function
static Vector<ComponentValue> replace_a_var_function(AbstractOrHypotheticalElement& element, GuardedSubstitutionContexts& guarded_contexts, ArbitrarySubstitutionReplacementContext const& replacement_context, ArbitrarySubstitutionFunctionArguments const& arguments)
{
    // 1. Let el be the element that the style containing the var() function is being applied to.
    //    Let first arg be the first <declaration-value> in arguments.
    //    Let second arg be the <declaration-value>? passed after the comma, or null if there was no comma.
    auto const& declaration_value_list = arguments.get<DeclarationValueList>();

    auto const& first_argument = declaration_value_list.first();
    auto const second_argument = declaration_value_list.get(1);

    // 2. Substitute arbitrary substitution functions in first arg, then parse it as a <custom-property-name>.
    //    If parsing returned a <custom-property-name>, let result be the computed value of the corresponding custom
    //    property on el. Otherwise, let result be the guaranteed-invalid value.
    auto substituted_first_argument = substitute_arbitrary_substitution_functions(element, guarded_contexts, replacement_context, first_argument);
    TokenStream name_tokens { substituted_first_argument };
    name_tokens.discard_whitespace();
    auto& name_token = name_tokens.consume_a_token();
    name_tokens.discard_whitespace();

    Vector<ComponentValue> result;
    if (name_tokens.has_next_token() || !name_token.is(Token::Type::Ident) || !is_a_custom_property_name_string(name_token.token().ident())) {
        result = { ComponentValue { GuaranteedInvalidValue {} } };
    } else {
        // Look up the value of the custom property
        auto custom_property_name = name_token.token().ident();
        element.abstract_element().element().record_style_custom_property_reference(custom_property_name);

        // NB: We compute against the element that declared the custom property (if any did) - this is irrelevant for
        //     normal style computation since inherited values are already in computed form (since style computation
        //     occurs in tree order), but it is required for custom function evaluation.
        auto element_to_compute_custom_property_against = [&] -> AbstractOrHypotheticalElement {
            auto looked_up_value = element.get_custom_property(custom_property_name);
            if (!looked_up_value)
                return element;

            // A final value computes the same against any element: it is already in computed form,
            // so nothing font-relative or function-shaped is left to resolve against the declarer.
            // Values in a hypothetical element's chain keep the walk, since a custom function's
            // locals and result carry semantics the declaring element decides.
            if (element.has<DOM::AbstractElement>()
                && !(looked_up_value->is_unresolved() && looked_up_value->as_unresolved().contains_arbitrary_substitution_function()))
                return element;

            Optional<AbstractOrHypotheticalElement> current_element = element;

            while (current_element.has_value()) {
                auto custom_property_data = current_element->custom_property_data();

                // NB: We disable own value absorption for CustomPropertyData associated with custom functions so this
                //     is a reliable way to check if the current element is the one that declared the custom property.
                if (custom_property_data && custom_property_data->own_values().contains(custom_property_name))
                    return current_element.value();

                current_element = current_element->element_to_inherit_style_from();
            }

            return element;
        }();

        // NB: We always use `replacement_context.computed_style_for_custom_property_resolution here` rather than the
        //     computed style of the element we are computing against because either:
        //      1. We are computing against either the current element, an ancestor hypothetical element, or the
        //         "calling context root" element, in which case this is the correct computed style to use, or;
        //      2. We are computing against an ancestor abstract element in which case the custom property's value will
        //         already be in it's computed form so the computed_style used is irrelevant.
        auto& style_computer = element.document().style_computer();
        auto custom_property_value = style_computer.compute_value_of_custom_property(replacement_context.computed_style_for_custom_property_resolution, element_to_compute_custom_property_against, custom_property_name, guarded_contexts);
        result = style_computer.tokenized_custom_property_value(custom_property_value);
    }

    // FIXME: 3. If the custom property named by the var()’s first argument is animation-tainted, and the var() is being used
    //    in a property that is not animatable, set result to the guaranteed-invalid value.

    // 4. If result contains the guaranteed-invalid value, and second arg was provided, set result to the result of substitute arbitrary substitution functions on second arg.
    if (contains_guaranteed_invalid_value(result) && second_argument.has_value())
        result = substitute_arbitrary_substitution_functions(element, guarded_contexts, replacement_context, second_argument.value());

    // 5. Return result.
    return result;
}

static ErrorOr<void> substitute_arbitrary_substitution_functions_step_2(AbstractOrHypotheticalElement& element, GuardedSubstitutionContexts& guarded_contexts, ArbitrarySubstitutionReplacementContext const& replacement_context, TokenStream<ComponentValue>& source, Vector<ComponentValue>& dest)
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
                auto result = replace_an_arbitrary_substitution_function(element, guarded_contexts, replacement_context, function_id, source_function.name, arguments);

                // 5. If result contains the guaranteed-invalid value, replace func in values with the guaranteed-invalid value.
                //    Otherwise, replace func in values with result.
                if (contains_guaranteed_invalid_value(result)) {
                    dest.empend(GuaranteedInvalidValue {});
                } else {
                    // NB: Because we're doing this in one pass recursively, we now need to substitute any ASFs in result.
                    TokenStream result_stream { result };
                    Vector<ComponentValue> result_after_processing;
                    TRY(substitute_arbitrary_substitution_functions_step_2(element, guarded_contexts, replacement_context, result_stream, result_after_processing));

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
            TRY(substitute_arbitrary_substitution_functions_step_2(element, guarded_contexts, replacement_context, source_function_contents, function_values));
            dest.empend(Function { source_function.name, move(function_values) });
            continue;
        }
        if (value.is_block()) {
            auto const& source_block = value.block();
            TokenStream source_block_values { source_block.value };
            Vector<ComponentValue> block_values;
            TRY(substitute_arbitrary_substitution_functions_step_2(element, guarded_contexts, replacement_context, source_block_values, block_values));
            dest.empend(SimpleBlock { source_block.token, move(block_values) });
            continue;
        }
        dest.empend(value);
    }

    return {};
}

// https://drafts.csswg.org/css-values-5/#substitute-arbitrary-substitution-function
Vector<ComponentValue> substitute_arbitrary_substitution_functions(AbstractOrHypotheticalElement& element, GuardedSubstitutionContexts& guarded_contexts, ArbitrarySubstitutionReplacementContext const& replacement_context, ReadonlySpan<ComponentValue> values, Optional<SubstitutionContext> context)
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
    auto maybe_error = substitute_arbitrary_substitution_functions_step_2(element, guarded_contexts, replacement_context, source, new_values);
    if (maybe_error.is_error())
        return { ComponentValue { GuaranteedInvalidValue {} } };

    // 3. If context is marked as a cyclic substitution context, return the guaranteed-invalid value.
    // NOTE: Nested arbitrary substitution functions may have marked context as cyclic in step 2.
    if (context.has_value() && context->is_cyclic)
        return { ComponentValue { GuaranteedInvalidValue {} } };

    // 4. Return values.
    return new_values;
}

// https://drafts.csswg.org/css-values-5/#replace-an-arbitrary-substitution-function
Vector<ComponentValue> replace_an_arbitrary_substitution_function(AbstractOrHypotheticalElement& element, GuardedSubstitutionContexts& guarded_contexts, ArbitrarySubstitutionReplacementContext const& replacement_context, ArbitrarySubstitutionFunction function, Utf16FlyString const& function_name, ArbitrarySubstitutionFunctionArguments const& arguments)
{
    switch (function) {
    case ArbitrarySubstitutionFunction::Attr:
        return replace_an_attr_function(element, guarded_contexts, replacement_context, arguments);
    case ArbitrarySubstitutionFunction::DashedFunction:
        return replace_a_dashed_function(element, guarded_contexts, replacement_context, function_name, arguments);
    case ArbitrarySubstitutionFunction::Env:
        return replace_an_env_function(element, guarded_contexts, replacement_context, arguments);
    case ArbitrarySubstitutionFunction::If:
        return replace_an_if_function(element, guarded_contexts, replacement_context, arguments);
    case ArbitrarySubstitutionFunction::Inherit:
        return replace_an_inherit_function(element, guarded_contexts, replacement_context, arguments);
    case ArbitrarySubstitutionFunction::Var:
        return replace_a_var_function(element, guarded_contexts, replacement_context, arguments);
    }
    VERIFY_NOT_REACHED();
}

NonnullRefPtr<StyleValue const> Parser::resolve_unresolved_style_value(ParsingParams const& context, AbstractOrHypotheticalElement element, ArbitrarySubstitutionReplacementContext const& replacement_context, PropertyNameAndID const& property, UnresolvedStyleValue const& unresolved, Optional<GuardedSubstitutionContexts&> existing_guarded_contexts)
{
    auto parser = Parser::create(context, ""sv);
    if (existing_guarded_contexts.has_value())
        return parser.resolve_unresolved_style_value(element, *existing_guarded_contexts, replacement_context, property, unresolved);
    GuardedSubstitutionContexts guarded_contexts;
    return parser.resolve_unresolved_style_value(element, guarded_contexts, replacement_context, property, unresolved);
}

OwnPtr<BooleanExpression> Parser::parse_boolean_expression(TokenStream<ComponentValue>& tokens, MatchResult result_for_general_enclosed, ParseTest parse_test)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    if (tokens.next_token().is_ident("not"_utf16)) {
        tokens.discard_a_token();
        tokens.discard_whitespace();
        if (auto child = parse_boolean_expression_group(tokens, result_for_general_enclosed, parse_test)) {
            tokens.discard_whitespace();
            transaction.commit();
            return BooleanNotExpression::create(child.release_nonnull());
        }
        return {};
    }

    Vector<NonnullOwnPtr<BooleanExpression>> children;
    enum class Combinator : u8 {
        And,
        Or,
    };
    Optional<Combinator> combinator;
    auto as_combinator = [](auto& token) -> Optional<Combinator> {
        if (!token.is(Token::Type::Ident))
            return {};
        auto ident = token.token().ident();
        if (ident.equals_ignoring_ascii_case("and"sv))
            return Combinator::And;
        if (ident.equals_ignoring_ascii_case("or"sv))
            return Combinator::Or;
        return {};
    };

    while (tokens.has_next_token()) {
        if (!children.is_empty()) {
            auto maybe_combinator = as_combinator(tokens.consume_a_token());
            if (!maybe_combinator.has_value())
                return {};
            if (!combinator.has_value())
                combinator = maybe_combinator.value();
            else if (maybe_combinator != combinator)
                return {};
        }

        tokens.discard_whitespace();
        if (auto child = parse_boolean_expression_group(tokens, result_for_general_enclosed, parse_test))
            children.append(child.release_nonnull());
        else
            return {};
        tokens.discard_whitespace();
    }

    if (children.is_empty())
        return {};
    transaction.commit();
    if (children.size() == 1)
        return children.take_first();
    VERIFY(combinator.has_value());
    return *combinator == Combinator::And
        ? OwnPtr<BooleanExpression> { BooleanAndExpression::create(move(children)) }
        : OwnPtr<BooleanExpression> { BooleanOrExpression::create(move(children)) };
}

OwnPtr<BooleanExpression> Parser::parse_boolean_expression_group(TokenStream<ComponentValue>& tokens, MatchResult result_for_general_enclosed, ParseTest parse_test)
{
    auto const& first_token = tokens.next_token();
    if (first_token.is_block() && first_token.block().is_paren()) {
        auto transaction = tokens.begin_transaction();
        tokens.discard_a_token();
        tokens.discard_whitespace();
        TokenStream child_tokens { first_token.block().value };
        if (auto expression = parse_boolean_expression(child_tokens, result_for_general_enclosed, parse_test)) {
            if (child_tokens.has_next_token())
                return {};
            transaction.commit();
            return BooleanExpressionInParens::create(expression.release_nonnull());
        }
    }

    if (auto test = parse_test(tokens))
        return test.release_nonnull();
    if (auto general_enclosed = parse_general_enclosed(tokens, result_for_general_enclosed))
        return general_enclosed.release_nonnull();
    return {};
}

OwnPtr<GeneralEnclosed> Parser::parse_general_enclosed(TokenStream<ComponentValue>& tokens, MatchResult result)
{
    auto contains_only_any_value = [](auto const& values, auto&& contains_only_any_value) -> bool {
        for (auto const& value : values) {
            if (value.is_function()) {
                if (!contains_only_any_value(value.function().value, contains_only_any_value))
                    return false;
                continue;
            }
            if (value.is_block()) {
                if (!contains_only_any_value(value.block().value, contains_only_any_value))
                    return false;
                continue;
            }
            if (!value.is_token())
                continue;
            switch (value.token().type()) {
            case Token::Type::Invalid:
            case Token::Type::EndOfFile:
            case Token::Type::BadString:
            case Token::Type::BadUrl:
            case Token::Type::Function:
            case Token::Type::OpenCurly:
            case Token::Type::OpenParen:
            case Token::Type::OpenSquare:
            case Token::Type::CloseCurly:
            case Token::Type::CloseParen:
            case Token::Type::CloseSquare:
                return false;
            default:
                break;
            }
        }
        return true;
    };

    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto const& first_token = tokens.consume_a_token();
    auto serialize = [](ComponentValue const& value) {
        auto original_source_text = value.original_source_text();
        return original_source_text.is_empty() ? value.to_string() : original_source_text;
    };
    if (first_token.is_function() && contains_only_any_value(first_token.function().value, contains_only_any_value)) {
        transaction.commit();
        return GeneralEnclosed::create(serialize(first_token), result);
    }
    if (first_token.is_block() && first_token.block().is_paren() && contains_only_any_value(first_token.block().value, contains_only_any_value)) {
        transaction.commit();
        return GeneralEnclosed::create(serialize(first_token), result);
    }
    return {};
}

OwnPtr<BooleanExpression> Parser::parse_if_condition(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.mark();
    auto expression = parse_boolean_expression(tokens, MatchResult::False, [&](TokenStream<ComponentValue>& test_tokens) -> OwnPtr<BooleanExpression> {
        auto const& token = test_tokens.consume_a_token();
        if (!token.is_function())
            return nullptr;
        auto const& function = token.function();
        auto source = serialize_a_series_of_component_values_preserving_original_source_text(function.value);
        if (function.name.equals_ignoring_ascii_case("supports"sv)) {
            if (auto declaration = RustQueryParser::parse_supports_declaration(*this, source))
                return declaration;
            return RustQueryParser::parse_supports_condition(*this, source);
        }
        if (function.name.equals_ignoring_ascii_case("media"sv)) {
            if (auto feature = RustQueryParser::parse_media_feature(*this, source))
                return feature;
            return RustQueryParser::parse_media_condition(*this, source);
        }
        if (function.name.equals_ignoring_ascii_case("style"sv))
            return RustQueryParser::parse_style_query(*this, source);
        return nullptr;
    });
    tokens.discard_whitespace();
    if (expression && !tokens.has_next_token()) {
        tokens.discard_a_mark();
        transaction.commit();
        return expression;
    }
    tokens.restore_a_mark();
    if (parse_all_as_single_keyword_value(tokens, Keyword::Else)) {
        transaction.commit();
        return ConstantBooleanExpression::create(MatchResult::True);
    }
    return nullptr;
}

NonnullRefPtr<StyleValue const> Parser::resolve_unresolved_style_value(AbstractOrHypotheticalElement element, GuardedSubstitutionContexts& guarded_contexts, ArbitrarySubstitutionReplacementContext const& replacement_context, PropertyNameAndID const& property, UnresolvedStyleValue const& unresolved)
{
    if (unresolved.includes_attr_function())
        element.abstract_element().element().set_style_uses_attr_css_function();
    if (unresolved.includes_if_function())
        element.abstract_element().element().set_style_uses_if_css_function();
    if (unresolved.includes_inherit_function())
        element.abstract_element().element().set_style_uses_inherit_css_function();
    if (unresolved.includes_var_function())
        element.abstract_element().element().set_style_uses_var_css_function();

    auto result = substitute_arbitrary_substitution_functions(element, guarded_contexts, replacement_context, unresolved_style_value_components(unresolved), SubstitutionContext { PropertySubstitutionContextDependency::create(property.name().to_utf16_string(), element) });
    if (contains_guaranteed_invalid_value(result))
        return GuaranteedInvalidStyleValue::create();
    if (property.is_custom_property()) {
        if (unresolved.contains_arbitrary_substitution_function()) {
            TokenStream keyword_tokens { result };
            keyword_tokens.discard_whitespace();
            if (keyword_tokens.has_next_token()) {
                auto const& token = keyword_tokens.consume_a_token();
                keyword_tokens.discard_whitespace();
                if (!keyword_tokens.has_next_token() && token.is(Token::Type::Ident)) {
                    auto keyword = keyword_from_string(token.token().ident());
                    if (keyword.has_value() && is_css_wide_keyword(*keyword))
                        return KeywordStyleValue::create(*keyword);
                }
            }
        }
        auto contains_attr_tainted_values = result.first_matching([](auto const& value) { return value.contains_attr_tainted_value(); }).has_value();
        auto source_text = serialize_a_series_of_component_values_preserving_original_source_text(result);
        source_text = unresolved.contains_arbitrary_substitution_function() && !contains_attr_tainted_values
            ? source_text.trim_ascii_whitespace(TrimMode::Left)
            : source_text.trim_ascii_whitespace();
        return UnresolvedStyleValue::create(move(source_text), {}, {}, UnresolvedStyleValue::SourceTextMode::Preserve, contains_attr_tainted_values);
    }
    TokenStream expanded_tokens { result };
    auto parsed = parse_css_value(property.id(), expanded_tokens, {}, ValueIsSubstituted::Yes);
    return parsed.is_error() ? GuaranteedInvalidStyleValue::create() : parsed.release_value();
}

}
