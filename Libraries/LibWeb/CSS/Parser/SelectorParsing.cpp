/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::CSS::Parser {

Optional<SelectorList> Parser::parse_as_selector(SelectorParsingMode parsing_mode)
{
    auto selector_list = parse_a_selector_list(m_token_stream, SelectorType::Standalone, parsing_mode);
    if (!selector_list.is_error())
        return selector_list.release_value();

    return {};
}

Optional<SelectorList> Parser::parse_as_relative_selector(SelectorParsingMode parsing_mode)
{
    auto selector_list = parse_a_selector_list(m_token_stream, SelectorType::Relative, parsing_mode);
    if (!selector_list.is_error())
        return selector_list.release_value();

    return {};
}

Optional<Selector::PseudoElementSelector> Parser::parse_as_pseudo_element_selector()
{
    // FIXME: This is quite janky. Selector parsing is not at all designed to allow parsing just a single part of a selector.
    //        So, this code parses a whole selector, then rejects it if it's not a single pseudo-element simple selector.
    //        Come back and fix this, future Sam!
    auto maybe_selector_list = parse_a_selector_list(m_token_stream, SelectorType::Standalone, SelectorParsingMode::Standard);
    if (maybe_selector_list.is_error())
        return {};
    auto& selector_list = maybe_selector_list.value();

    if (selector_list.size() != 1)
        return {};
    auto& selector = selector_list.first();

    if (selector->compound_selectors().size() != 1)
        return {};
    auto& first_compound_selector = selector->compound_selectors().first();

    if (first_compound_selector.simple_selectors.size() != 1)
        return {};
    auto& simple_selector = first_compound_selector.simple_selectors.first();

    if (simple_selector.type != Selector::SimpleSelector::Type::PseudoElement)
        return {};

    return simple_selector.pseudo_element();
}

static NonnullRefPtr<Selector> create_invalid_selector(Selector::Combinator combinator, Vector<ComponentValue> component_values)
{
    // Trim leading and trailing whitespace
    while (!component_values.is_empty() && component_values.first().is(Token::Type::Whitespace)) {
        component_values.take_first();
    }
    while (!component_values.is_empty() && component_values.last().is(Token::Type::Whitespace)) {
        component_values.take_last();
    }

    Selector::SimpleSelector simple {
        .type = Selector::SimpleSelector::Type::Invalid,
        .value = Selector::SimpleSelector::Invalid {
            .component_values = move(component_values),
        }
    };
    Selector::CompoundSelector compound {
        .combinator = combinator,
        .simple_selectors = { move(simple) }
    };
    return Selector::create({ move(compound) });
}

template<typename T>
Parser::ParseErrorOr<SelectorList> Parser::parse_a_selector_list(TokenStream<T>& tokens, SelectorType mode, SelectorParsingMode parsing_mode)
{
    SelectorList selectors;
    for (;;) {
        auto selector_parts = consume_a_list_of_component_values(tokens, Token::Type::Comma);
        auto stream = TokenStream(selector_parts);
        auto selector = parse_complex_selector(stream, mode);
        if (selector.is_error()) {
            if (parsing_mode == SelectorParsingMode::Forgiving) {
                // Keep the invalid selector around for serialization and nesting
                auto combinator = mode == SelectorType::Standalone ? Selector::Combinator::None : Selector::Combinator::Descendant;
                selectors.append(create_invalid_selector(combinator, move(selector_parts)));
            } else {
                return selector.error();
            }
        } else {
            selectors.append(selector.release_value());
        }

        if (tokens.is_empty())
            break;

        tokens.discard_a_token();
    }

    if (selectors.is_empty() && parsing_mode != SelectorParsingMode::Forgiving)
        return ParseError::SyntaxError;

    return selectors;
}
template Parser::ParseErrorOr<SelectorList> Parser::parse_a_selector_list(TokenStream<ComponentValue>&, SelectorType, SelectorParsingMode);
template Parser::ParseErrorOr<SelectorList> Parser::parse_a_selector_list(TokenStream<Token>&, SelectorType, SelectorParsingMode);

Parser::ParseErrorOr<NonnullRefPtr<Selector>> Parser::parse_complex_selector(TokenStream<ComponentValue>& tokens, SelectorType mode)
{
    Vector<Selector::CompoundSelector> compound_selectors;

    auto first_selector = TRY(parse_compound_selector(tokens));
    if (!first_selector.has_value())
        return ParseError::SyntaxError;

    if (mode == SelectorType::Standalone) {
        if (first_selector->combinator != Selector::Combinator::Descendant)
            return ParseError::SyntaxError;
        first_selector->combinator = Selector::Combinator::None;
    }
    compound_selectors.append(first_selector.release_value());

    while (tokens.has_next_token()) {
        auto compound_selector = TRY(parse_compound_selector(tokens));
        if (!compound_selector.has_value())
            break;
        compound_selectors.append(compound_selector.release_value());
    }

    if (compound_selectors.is_empty())
        return ParseError::SyntaxError;

    return Selector::create(move(compound_selectors));
}

Parser::ParseErrorOr<Optional<Selector::CompoundSelector>> Parser::parse_compound_selector(TokenStream<ComponentValue>& tokens)
{
    tokens.discard_whitespace();

    auto combinator = parse_selector_combinator(tokens).value_or(Selector::Combinator::Descendant);

    tokens.discard_whitespace();

    Vector<Selector::SimpleSelector> simple_selectors;

    while (tokens.has_next_token()) {
        auto component = TRY(parse_simple_selector(tokens));
        if (!component.has_value())
            break;
        if (component->type == Selector::SimpleSelector::Type::TagName && !simple_selectors.is_empty()) {
            // Tag-name selectors can only go at the beginning of a compound selector.
            return ParseError::SyntaxError;
        }
        simple_selectors.append(component.release_value());
    }

    if (simple_selectors.is_empty()) {
        if (tokens.has_next_token() || combinator != Selector::Combinator::Descendant)
            return ParseError::SyntaxError;

        return Optional<Selector::CompoundSelector> {};
    }

    return Selector::CompoundSelector { combinator, move(simple_selectors) };
}

Optional<Selector::Combinator> Parser::parse_selector_combinator(TokenStream<ComponentValue>& tokens)
{
    auto const& current_value = tokens.consume_a_token();
    if (current_value.is(Token::Type::Delim)) {
        switch (current_value.token().delim()) {
        case '>':
            return Selector::Combinator::ImmediateChild;
        case '+':
            return Selector::Combinator::NextSibling;
        case '~':
            return Selector::Combinator::SubsequentSibling;
        case '|': {
            auto const& next = tokens.next_token();
            if (next.is(Token::Type::EndOfFile))
                return {};

            if (next.is_delim('|')) {
                tokens.discard_a_token();
                return Selector::Combinator::Column;
            }
        }
        }
    }

    tokens.reconsume_current_input_token();
    return {};
}

Optional<Selector::SimpleSelector::QualifiedName> Parser::parse_selector_qualified_name(TokenStream<ComponentValue>& tokens, AllowWildcardName allow_wildcard_name)
{
    auto is_name = [](ComponentValue const& token) {
        return token.is_delim('*') || token.is(Token::Type::Ident);
    };
    auto get_name = [](ComponentValue const& token) {
        if (token.is_delim('*'))
            return "*"_fly_string;
        return token.token().ident();
    };

    // There are 3 possibilities here:
    // (Where <name> and <namespace> are either an <ident> or a `*` delim)
    // 1) `|<name>`
    // 2) `<namespace>|<name>`
    // 3) `<name>`
    // Whitespace is forbidden between any of these parts. https://www.w3.org/TR/selectors-4/#white-space

    auto transaction = tokens.begin_transaction();

    auto const& first_token = tokens.consume_a_token();
    if (first_token.is_delim('|')) {
        // Case 1: `|<name>`
        if (is_name(tokens.next_token())) {
            auto const& name_token = tokens.consume_a_token();

            if (allow_wildcard_name == AllowWildcardName::No && name_token.is_delim('*'))
                return {};

            transaction.commit();
            return Selector::SimpleSelector::QualifiedName {
                .namespace_type = Selector::SimpleSelector::QualifiedName::NamespaceType::None,
                .name = get_name(name_token),
            };
        }
        return {};
    }

    if (!is_name(first_token))
        return {};

    if (tokens.next_token().is_delim('|') && is_name(tokens.peek_token(1))) {
        // Case 2: `<namespace>|<name>`
        tokens.discard_a_token(); // `|`
        auto namespace_ = get_name(first_token);
        auto name = get_name(tokens.consume_a_token());

        if (allow_wildcard_name == AllowWildcardName::No && name == "*"sv)
            return {};

        auto namespace_type = namespace_ == "*"sv
            ? Selector::SimpleSelector::QualifiedName::NamespaceType::Any
            : Selector::SimpleSelector::QualifiedName::NamespaceType::Named;

        transaction.commit();
        return Selector::SimpleSelector::QualifiedName {
            .namespace_type = namespace_type,
            .namespace_ = namespace_,
            .name = name,
        };
    }

    // Case 3: `<name>`
    auto& name_token = first_token;
    if (allow_wildcard_name == AllowWildcardName::No && name_token.is_delim('*'))
        return {};

    transaction.commit();
    return Selector::SimpleSelector::QualifiedName {
        .namespace_type = Selector::SimpleSelector::QualifiedName::NamespaceType::Default,
        .name = get_name(name_token),
    };
}

Parser::ParseErrorOr<Selector::SimpleSelector> Parser::parse_attribute_simple_selector(ComponentValue const& first_value)
{
    auto attribute_tokens = TokenStream { first_value.block().value };

    attribute_tokens.discard_whitespace();

    if (!attribute_tokens.has_next_token()) {
        dbgln_if(CSS_PARSER_DEBUG, "CSS attribute selector is empty!");
        return ParseError::SyntaxError;
    }

    auto maybe_qualified_name = parse_selector_qualified_name(attribute_tokens, AllowWildcardName::No);
    if (!maybe_qualified_name.has_value()) {
        dbgln_if(CSS_PARSER_DEBUG, "Expected qualified-name for attribute name, got: '{}'", attribute_tokens.next_token().to_debug_string());
        return ParseError::SyntaxError;
    }
    auto qualified_name = maybe_qualified_name.release_value();

    Selector::SimpleSelector simple_selector {
        .type = Selector::SimpleSelector::Type::Attribute,
        .value = Selector::SimpleSelector::Attribute {
            .match_type = Selector::SimpleSelector::Attribute::MatchType::HasAttribute,
            .qualified_name = qualified_name,
            .case_type = Selector::SimpleSelector::Attribute::CaseType::DefaultMatch,
        }
    };

    attribute_tokens.discard_whitespace();
    if (!attribute_tokens.has_next_token())
        return simple_selector;

    auto const& delim_part = attribute_tokens.consume_a_token();
    if (!delim_part.is(Token::Type::Delim)) {
        dbgln_if(CSS_PARSER_DEBUG, "Expected a delim for attribute comparison, got: '{}'", delim_part.to_debug_string());
        return ParseError::SyntaxError;
    }

    if (delim_part.token().delim() == '=') {
        simple_selector.attribute().match_type = Selector::SimpleSelector::Attribute::MatchType::ExactValueMatch;
    } else {
        if (!attribute_tokens.has_next_token()) {
            dbgln_if(CSS_PARSER_DEBUG, "Attribute selector ended part way through a match type.");
            return ParseError::SyntaxError;
        }

        auto const& delim_second_part = attribute_tokens.consume_a_token();
        if (!delim_second_part.is_delim('=')) {
            dbgln_if(CSS_PARSER_DEBUG, "Expected a double delim for attribute comparison, got: '{}{}'", delim_part.to_debug_string(), delim_second_part.to_debug_string());
            return ParseError::SyntaxError;
        }
        switch (delim_part.token().delim()) {
        case '~':
            simple_selector.attribute().match_type = Selector::SimpleSelector::Attribute::MatchType::ContainsWord;
            break;
        case '*':
            simple_selector.attribute().match_type = Selector::SimpleSelector::Attribute::MatchType::ContainsString;
            break;
        case '|':
            simple_selector.attribute().match_type = Selector::SimpleSelector::Attribute::MatchType::StartsWithSegment;
            break;
        case '^':
            simple_selector.attribute().match_type = Selector::SimpleSelector::Attribute::MatchType::StartsWithString;
            break;
        case '$':
            simple_selector.attribute().match_type = Selector::SimpleSelector::Attribute::MatchType::EndsWithString;
            break;
        default:
            attribute_tokens.reconsume_current_input_token();
        }
    }

    attribute_tokens.discard_whitespace();
    if (!attribute_tokens.has_next_token()) {
        dbgln_if(CSS_PARSER_DEBUG, "Attribute selector ended without a value to match.");
        return ParseError::SyntaxError;
    }

    auto const& value_part = attribute_tokens.consume_a_token();
    if (!value_part.is(Token::Type::Ident) && !value_part.is(Token::Type::String)) {
        dbgln_if(CSS_PARSER_DEBUG, "Expected a string or ident for the value to match attribute against, got: '{}'", value_part.to_debug_string());
        return ParseError::SyntaxError;
    }
    auto const& value_string = value_part.token().is(Token::Type::Ident) ? value_part.token().ident() : value_part.token().string();
    simple_selector.attribute().value = value_string.to_string();

    attribute_tokens.discard_whitespace();
    // Handle case-sensitivity suffixes. https://www.w3.org/TR/selectors-4/#attribute-case
    if (attribute_tokens.has_next_token()) {
        auto const& case_sensitivity_part = attribute_tokens.consume_a_token();
        if (case_sensitivity_part.is(Token::Type::Ident)) {
            auto case_sensitivity = case_sensitivity_part.token().ident();
            if (case_sensitivity.equals_ignoring_ascii_case("i"sv)) {
                simple_selector.attribute().case_type = Selector::SimpleSelector::Attribute::CaseType::CaseInsensitiveMatch;
            } else if (case_sensitivity.equals_ignoring_ascii_case("s"sv)) {
                simple_selector.attribute().case_type = Selector::SimpleSelector::Attribute::CaseType::CaseSensitiveMatch;
            } else {
                dbgln_if(CSS_PARSER_DEBUG, "Expected a \"i\" or \"s\" attribute selector case sensitivity identifier, got: '{}'", case_sensitivity_part.to_debug_string());
                return ParseError::SyntaxError;
            }
        } else {
            dbgln_if(CSS_PARSER_DEBUG, "Expected an attribute selector case sensitivity identifier, got: '{}'", case_sensitivity_part.to_debug_string());
            return ParseError::SyntaxError;
        }
    }

    if (attribute_tokens.has_next_token()) {
        dbgln_if(CSS_PARSER_DEBUG, "Was not expecting anything else inside attribute selector.");
        return ParseError::SyntaxError;
    }

    return simple_selector;
}

Parser::ParseErrorOr<Selector::SimpleSelector> Parser::parse_pseudo_simple_selector(TokenStream<ComponentValue>& tokens)
{
    auto peek_token_ends_selector = [&]() -> bool {
        auto const& value = tokens.next_token();
        return (value.is(Token::Type::EndOfFile) || value.is(Token::Type::Whitespace) || value.is(Token::Type::Comma));
    };

    if (peek_token_ends_selector())
        return ParseError::SyntaxError;

    // Note that we already consumed one colon before we entered this function.
    // FIXME: Don't do that.
    bool is_pseudo_element = false;
    if (tokens.next_token().is(Token::Type::Colon)) {
        is_pseudo_element = true;
        tokens.discard_a_token();
        if (peek_token_ends_selector())
            return ParseError::SyntaxError;
    }

    if (is_pseudo_element) {
        auto const& name_token = tokens.consume_a_token();
        bool is_function = false;
        FlyString pseudo_name;

        if (name_token.is(Token::Type::Ident)) {
            pseudo_name = name_token.token().ident();
        } else if (name_token.is_function()) {
            pseudo_name = name_token.function().name;
            is_function = true;
        } else {
            dbgln_if(CSS_PARSER_DEBUG, "Expected an ident or function token for pseudo-element, got: '{}'", name_token.to_debug_string());
            return ParseError::SyntaxError;
        }

        bool is_aliased_pseudo = false;
        auto pseudo_element = pseudo_element_from_string(pseudo_name);
        if (!pseudo_element.has_value()) {
            pseudo_element = aliased_pseudo_element_from_string(pseudo_name);
            is_aliased_pseudo = pseudo_element.has_value();
        }

        if (pseudo_element.has_value()) {
            auto metadata = pseudo_element_metadata(*pseudo_element);

            // :has() is fussy about pseudo-elements inside it
            if (m_pseudo_class_context.contains_slow(PseudoClass::Has) && !is_has_allowed_pseudo_element(*pseudo_element)) {
                return ParseError::SyntaxError;
            }

            Selector::PseudoElementSelector::Value value = Empty {};
            if (is_function) {
                if (!metadata.is_valid_as_function) {
                    dbgln_if(CSS_PARSER_DEBUG, "Pseudo-element '::{}()' is not valid as a function.", pseudo_name);
                    return ParseError::SyntaxError;
                }

                // Parse arguments
                TokenStream function_tokens { name_token.function().value };
                function_tokens.discard_whitespace();

                switch (metadata.parameter_type) {
                case PseudoElementMetadata::ParameterType::None:
                    if (function_tokens.has_next_token()) {
                        dbgln_if(CSS_PARSER_DEBUG, "Pseudo-element '::{}()' invalid: Should have no arguments.", pseudo_name);
                        return ParseError::SyntaxError;
                    }
                    break;
                case PseudoElementMetadata::ParameterType::PTNameSelector: {
                    // <pt-name-selector> = '*' | <custom-ident>
                    // https://drafts.csswg.org/css-view-transitions-1/#typedef-pt-name-selector
                    if (function_tokens.next_token().is_delim('*')) {
                        function_tokens.discard_a_token(); // *
                        value = Selector::PseudoElementSelector::PTNameSelector { .is_universal = true };
                    } else if (auto custom_ident = parse_custom_ident(function_tokens, {}); custom_ident.has_value()) {
                        value = Selector::PseudoElementSelector::PTNameSelector { .value = custom_ident.release_value() };
                    } else {
                        dbgln_if(CSS_PARSER_DEBUG, "Invalid <pt-name-selector> in :{}() - expected `*` or `<custom-ident>`, got `{}`", pseudo_name, function_tokens.next_token().to_debug_string());
                        return ParseError::SyntaxError;
                    }
                    function_tokens.discard_whitespace();
                    if (function_tokens.has_next_token()) {
                        dbgln_if(CSS_PARSER_DEBUG, "Invalid <pt-name-selector> in :{}() - trailing tokens", pseudo_name);
                        return ParseError::SyntaxError;
                    }
                    break;
                }
                }

            } else {
                if (!metadata.is_valid_as_identifier) {
                    dbgln_if(CSS_PARSER_DEBUG, "Pseudo-element '::{}' is not valid as an identifier.", pseudo_name);
                    return ParseError::SyntaxError;
                }
            }

            // Aliased pseudo-elements behave like their target pseudo-element, but serialize as themselves. So store their
            // name like we do for unknown -webkit pseudos below.
            if (is_aliased_pseudo) {
                return Selector::SimpleSelector {
                    .type = Selector::SimpleSelector::Type::PseudoElement,
                    .value = Selector::PseudoElementSelector { pseudo_element.release_value(), pseudo_name.to_string().to_ascii_lowercase(), move(value) }
                };
            }

            return Selector::SimpleSelector {
                .type = Selector::SimpleSelector::Type::PseudoElement,
                .value = Selector::PseudoElementSelector { pseudo_element.release_value(), move(value) }
            };
        }

        // https://www.w3.org/TR/selectors-4/#compat
        // All other pseudo-elements whose names begin with the string “-webkit-” (matched ASCII case-insensitively)
        // and that are not functional notations must be treated as valid at parse time. (That is, ::-webkit-asdf is
        // valid at parse time, but ::-webkit-jkl() is not.) If they’re not otherwise recognized and supported, they
        // must be treated as matching nothing, and are unknown -webkit- pseudo-elements.
        if (!is_function && pseudo_name.starts_with_bytes("-webkit-"sv, CaseSensitivity::CaseInsensitive)) {
            // :has() only allows a limited set of pseudo-elements inside it, which doesn't include unknown ones.
            if (m_pseudo_class_context.contains_slow(PseudoClass::Has))
                return ParseError::SyntaxError;

            return Selector::SimpleSelector {
                .type = Selector::SimpleSelector::Type::PseudoElement,
                // Unknown -webkit- pseudo-elements must be serialized in ASCII lowercase.
                .value = Selector::PseudoElementSelector { PseudoElement::UnknownWebKit, pseudo_name.to_string().to_ascii_lowercase() },
            };
        }

        if (has_ignored_vendor_prefix(pseudo_name))
            return ParseError::IncludesIgnoredVendorPrefix;

        dbgln_if(CSS_PARSER_DEBUG, "Unrecognized pseudo-element: '::{}'", pseudo_name);
        return ParseError::SyntaxError;
    }

    if (peek_token_ends_selector())
        return ParseError::SyntaxError;

    auto const& pseudo_class_token = tokens.consume_a_token();

    if (pseudo_class_token.is(Token::Type::Ident)) {
        auto pseudo_name = pseudo_class_token.token().ident();
        if (has_ignored_vendor_prefix(pseudo_name))
            return ParseError::IncludesIgnoredVendorPrefix;

        auto make_pseudo_class_selector = [](auto pseudo_class) {
            return Selector::SimpleSelector {
                .type = Selector::SimpleSelector::Type::PseudoClass,
                .value = Selector::SimpleSelector::PseudoClassSelector { .type = pseudo_class }
            };
        };

        if (auto pseudo_class = pseudo_class_from_string(pseudo_name); pseudo_class.has_value()) {
            if (!pseudo_class_metadata(pseudo_class.value()).is_valid_as_identifier) {
                dbgln_if(CSS_PARSER_DEBUG, "Pseudo-class ':{}' is only valid as a function", pseudo_name);
                return ParseError::SyntaxError;
            }
            return make_pseudo_class_selector(pseudo_class.value());
        }

        // Single-colon syntax allowed for ::after, ::before, ::first-letter and ::first-line for compatibility.
        // https://www.w3.org/TR/selectors/#pseudo-element-syntax
        if (auto pseudo_element = pseudo_element_from_string(pseudo_name); pseudo_element.has_value()) {
            switch (pseudo_element.value()) {
            case PseudoElement::After:
            case PseudoElement::Before:
            case PseudoElement::FirstLetter:
            case PseudoElement::FirstLine:
                // :has() is fussy about pseudo-elements inside it
                if (m_pseudo_class_context.contains_slow(PseudoClass::Has) && !is_has_allowed_pseudo_element(pseudo_element.value())) {
                    return ParseError::SyntaxError;
                }

                return Selector::SimpleSelector {
                    .type = Selector::SimpleSelector::Type::PseudoElement,
                    .value = Selector::PseudoElementSelector { pseudo_element.value() }
                };
            default:
                break;
            }
        }

        dbgln_if(CSS_PARSER_DEBUG, "Unrecognized pseudo-class: ':{}'", pseudo_name);
        return ParseError::SyntaxError;
    }

    if (pseudo_class_token.is_function()) {
        auto parse_nth_child_selector = [this](auto pseudo_class, Vector<ComponentValue> const& function_values, bool allow_of = false) -> ParseErrorOr<Selector::SimpleSelector> {
            auto tokens = TokenStream<ComponentValue>(function_values);
            auto nth_child_pattern = parse_a_n_plus_b_pattern(tokens);
            if (!nth_child_pattern.has_value()) {
                dbgln_if(CSS_PARSER_DEBUG, "!!! Invalid An+B format for {}", pseudo_class_name(pseudo_class));
                return ParseError::SyntaxError;
            }

            tokens.discard_whitespace();
            if (!tokens.has_next_token()) {
                return Selector::SimpleSelector {
                    .type = Selector::SimpleSelector::Type::PseudoClass,
                    .value = Selector::SimpleSelector::PseudoClassSelector {
                        .type = pseudo_class,
                        .nth_child_pattern = nth_child_pattern.release_value() }
                };
            }

            if (!allow_of)
                return ParseError::SyntaxError;

            // Parse the `of <selector-list>` syntax
            auto const& maybe_of = tokens.consume_a_token();
            if (!maybe_of.is_ident("of"sv))
                return ParseError::SyntaxError;

            tokens.discard_whitespace();
            auto selector_list = TRY(parse_a_selector_list(tokens, SelectorType::Standalone));

            tokens.discard_whitespace();
            if (tokens.has_next_token())
                return ParseError::SyntaxError;

            return Selector::SimpleSelector {
                .type = Selector::SimpleSelector::Type::PseudoClass,
                .value = Selector::SimpleSelector::PseudoClassSelector {
                    .type = pseudo_class,
                    .nth_child_pattern = nth_child_pattern.release_value(),
                    .argument_selector_list = move(selector_list) }
            };
        };

        auto const& pseudo_function = pseudo_class_token.function();
        auto maybe_pseudo_class = pseudo_class_from_string(pseudo_function.name);
        if (!maybe_pseudo_class.has_value()) {
            dbgln_if(CSS_PARSER_DEBUG, "Unrecognized pseudo-class function: ':{}'()", pseudo_function.name);
            return ParseError::SyntaxError;
        }
        auto pseudo_class = maybe_pseudo_class.value();
        auto metadata = pseudo_class_metadata(pseudo_class);

        if (!metadata.is_valid_as_function) {
            dbgln_if(CSS_PARSER_DEBUG, "Pseudo-class ':{}' is not valid as a function", pseudo_function.name);
            return ParseError::SyntaxError;
        }

        if (pseudo_function.value.is_empty()) {
            dbgln_if(CSS_PARSER_DEBUG, "Empty :{}() selector", pseudo_function.name);
            return ParseError::SyntaxError;
        }

        // "The :has() pseudo-class cannot be nested; :has() is not valid within :has()."
        // https://drafts.csswg.org/selectors/#relational
        if (pseudo_class == PseudoClass::Has && m_pseudo_class_context.contains_slow(PseudoClass::Has)) {
            dbgln_if(CSS_PARSER_DEBUG, ":has() is not allowed inside :has()");
            return ParseError::SyntaxError;
        }

        m_pseudo_class_context.append(pseudo_class);
        ScopeGuard guard = [&] { m_pseudo_class_context.take_last(); };

        switch (metadata.parameter_type) {
        case PseudoClassMetadata::ParameterType::ANPlusB:
            return parse_nth_child_selector(pseudo_class, pseudo_function.value, false);
        case PseudoClassMetadata::ParameterType::ANPlusBOf:
            return parse_nth_child_selector(pseudo_class, pseudo_function.value, true);
        case PseudoClassMetadata::ParameterType::CompoundSelector: {
            auto function_token_stream = TokenStream(pseudo_function.value);
            auto compound_selector_or_error = parse_compound_selector(function_token_stream);
            if (compound_selector_or_error.is_error() || !compound_selector_or_error.value().has_value()) {
                dbgln_if(CSS_PARSER_DEBUG, "Failed to parse :{}() parameter as a compound selector", pseudo_function.name);
                return ParseError::SyntaxError;
            }

            auto compound_selector = compound_selector_or_error.release_value().release_value();
            compound_selector.combinator = Selector::Combinator::None;

            Vector compound_selectors { move(compound_selector) };
            auto selector = Selector::create(move(compound_selectors));

            return Selector::SimpleSelector {
                .type = Selector::SimpleSelector::Type::PseudoClass,
                .value = Selector::SimpleSelector::PseudoClassSelector {
                    .type = pseudo_class,
                    .argument_selector_list = { move(selector) } }
            };
        }
        case PseudoClassMetadata::ParameterType::ForgivingRelativeSelectorList:
        case PseudoClassMetadata::ParameterType::ForgivingSelectorList: {
            auto function_token_stream = TokenStream(pseudo_function.value);
            auto selector_type = metadata.parameter_type == PseudoClassMetadata::ParameterType::ForgivingSelectorList
                ? SelectorType::Standalone
                : SelectorType::Relative;
            // NOTE: Because it's forgiving, even complete garbage will parse OK as an empty selector-list.
            auto argument_selector_list = MUST(parse_a_selector_list(function_token_stream, selector_type, SelectorParsingMode::Forgiving));

            return Selector::SimpleSelector {
                .type = Selector::SimpleSelector::Type::PseudoClass,
                .value = Selector::SimpleSelector::PseudoClassSelector {
                    .type = pseudo_class,
                    .is_forgiving = true,
                    .argument_selector_list = move(argument_selector_list) }
            };
        }
        case PseudoClassMetadata::ParameterType::Ident: {
            auto function_token_stream = TokenStream(pseudo_function.value);
            function_token_stream.discard_whitespace();
            auto const& maybe_keyword_token = function_token_stream.consume_a_token();
            function_token_stream.discard_whitespace();
            if (!maybe_keyword_token.is(Token::Type::Ident) || function_token_stream.has_next_token()) {
                dbgln_if(CSS_PARSER_DEBUG, "Failed to parse :{}() parameter as a keyword: not an ident", pseudo_function.name);
                return ParseError::SyntaxError;
            }

            auto maybe_keyword = keyword_from_string(maybe_keyword_token.token().ident());
            if (!maybe_keyword.has_value()) {
                dbgln_if(CSS_PARSER_DEBUG, "Failed to parse :{}() parameter as a keyword: unrecognized keyword", pseudo_function.name);
                return ParseError::SyntaxError;
            }

            return Selector::SimpleSelector {
                .type = Selector::SimpleSelector::Type::PseudoClass,
                .value = Selector::SimpleSelector::PseudoClassSelector {
                    .type = pseudo_class,
                    .keyword = maybe_keyword.value() }
            };
        }
        case PseudoClassMetadata::ParameterType::LanguageRanges: {
            Vector<FlyString> languages;
            auto function_token_stream = TokenStream(pseudo_function.value);
            auto language_token_lists = parse_a_comma_separated_list_of_component_values(function_token_stream);

            for (auto const& language_token_list : language_token_lists) {
                auto language_token_stream = TokenStream(language_token_list);
                language_token_stream.discard_whitespace();
                auto const& language_token = language_token_stream.consume_a_token();
                if (!(language_token.is(Token::Type::Ident) || language_token.is(Token::Type::String))) {
                    dbgln_if(CSS_PARSER_DEBUG, "Invalid language range in :{}() - not a string/ident", pseudo_function.name);
                    return ParseError::SyntaxError;
                }

                auto language_string = language_token.is(Token::Type::String) ? language_token.token().string() : language_token.token().ident();
                languages.append(language_string);

                language_token_stream.discard_whitespace();
                if (language_token_stream.has_next_token()) {
                    dbgln_if(CSS_PARSER_DEBUG, "Invalid language range in :{}() - trailing tokens", pseudo_function.name);
                    return ParseError::SyntaxError;
                }
            }

            return Selector::SimpleSelector {
                .type = Selector::SimpleSelector::Type::PseudoClass,
                .value = Selector::SimpleSelector::PseudoClassSelector {
                    .type = pseudo_class,
                    .languages = move(languages) }
            };
        }
        case PseudoClassMetadata::ParameterType::RelativeSelectorList:
        case PseudoClassMetadata::ParameterType::SelectorList: {
            auto function_token_stream = TokenStream(pseudo_function.value);
            auto selector_type = metadata.parameter_type == PseudoClassMetadata::ParameterType::SelectorList
                ? SelectorType::Standalone
                : SelectorType::Relative;
            auto not_selector = TRY(parse_a_selector_list(function_token_stream, selector_type));

            return Selector::SimpleSelector {
                .type = Selector::SimpleSelector::Type::PseudoClass,
                .value = Selector::SimpleSelector::PseudoClassSelector {
                    .type = pseudo_class,
                    .argument_selector_list = move(not_selector) }
            };
        }
        case PseudoClassMetadata::ParameterType::None:
            // `None` means this is not a function-type pseudo-class, so this state should be impossible.
            VERIFY_NOT_REACHED();
        }
    }
    dbgln_if(CSS_PARSER_DEBUG, "Unexpected Block in pseudo-class name, expected a function or identifier. '{}'", pseudo_class_token.to_debug_string());
    return ParseError::SyntaxError;
}

Parser::ParseErrorOr<Optional<Selector::SimpleSelector>> Parser::parse_simple_selector(TokenStream<ComponentValue>& tokens)
{
    auto peek_token_ends_selector = [&]() -> bool {
        auto const& value = tokens.next_token();
        return (value.is(Token::Type::EndOfFile) || value.is(Token::Type::Whitespace) || value.is(Token::Type::Comma));
    };

    if (peek_token_ends_selector())
        return Optional<Selector::SimpleSelector> {};

    // Handle universal and tag-name types together, since both can be namespaced
    if (auto qualified_name = parse_selector_qualified_name(tokens, AllowWildcardName::Yes); qualified_name.has_value()) {
        if (qualified_name->name.name == "*"sv) {
            return Selector::SimpleSelector {
                .type = Selector::SimpleSelector::Type::Universal,
                .value = qualified_name.release_value(),
            };
        }
        return Selector::SimpleSelector {
            .type = Selector::SimpleSelector::Type::TagName,
            .value = qualified_name.release_value(),
        };
    }

    auto const& first_value = tokens.consume_a_token();

    if (first_value.is(Token::Type::Delim)) {
        u32 delim = first_value.token().delim();
        switch (delim) {
        case '*':
            // Handled already
            VERIFY_NOT_REACHED();
        case '&':
            return Selector::SimpleSelector {
                .type = Selector::SimpleSelector::Type::Nesting,
            };
        case '.': {
            if (peek_token_ends_selector())
                return ParseError::SyntaxError;

            auto const& class_name_value = tokens.consume_a_token();
            if (!class_name_value.is(Token::Type::Ident)) {
                dbgln_if(CSS_PARSER_DEBUG, "Expected an ident after '.', got: {}", class_name_value.to_debug_string());
                return ParseError::SyntaxError;
            }
            return Selector::SimpleSelector {
                .type = Selector::SimpleSelector::Type::Class,
                .value = Selector::SimpleSelector::Name { class_name_value.token().ident() }
            };
        }
        case '>':
        case '+':
        case '~':
        case '|':
            // Whitespace is not required between the compound-selector and a combinator.
            // So, if we see a combinator, return that this compound-selector is done, instead of a syntax error.
            tokens.reconsume_current_input_token();
            return Optional<Selector::SimpleSelector> {};
        default:
            dbgln_if(CSS_PARSER_DEBUG, "!!! Invalid simple selector!");
            return ParseError::SyntaxError;
        }
    }

    if (first_value.is(Token::Type::Hash)) {
        if (first_value.token().hash_type() != Token::HashType::Id) {
            dbgln_if(CSS_PARSER_DEBUG, "Selector contains hash token that is not an id: {}", first_value.to_debug_string());
            return ParseError::SyntaxError;
        }
        return Selector::SimpleSelector {
            .type = Selector::SimpleSelector::Type::Id,
            .value = Selector::SimpleSelector::Name { first_value.token().hash_value() }
        };
    }

    if (first_value.is_block() && first_value.block().is_square())
        return TRY(parse_attribute_simple_selector(first_value));

    if (first_value.is(Token::Type::Colon))
        return TRY(parse_pseudo_simple_selector(tokens));

    dbgln_if(CSS_PARSER_DEBUG, "!!! Invalid simple selector!");
    return ParseError::SyntaxError;
}

Optional<Selector::SimpleSelector::ANPlusBPattern> Parser::parse_a_n_plus_b_pattern(TokenStream<ComponentValue>& values)
{
    auto transaction = values.begin_transaction();
    auto syntax_error = [&]() -> Optional<Selector::SimpleSelector::ANPlusBPattern> {
        if constexpr (CSS_PARSER_DEBUG) {
            dbgln_if(CSS_PARSER_DEBUG, "Invalid An+B value:");
            values.dump_all_tokens();
        }
        return {};
    };

    auto is_sign = [](ComponentValue const& value) -> bool {
        return value.is(Token::Type::Delim) && (value.token().delim() == '+' || value.token().delim() == '-');
    };
    auto is_n_dimension = [](ComponentValue const& value) -> bool {
        if (!value.is(Token::Type::Dimension))
            return false;
        if (!value.token().number().is_integer())
            return false;
        if (!value.token().dimension_unit().equals_ignoring_ascii_case("n"sv))
            return false;
        return true;
    };
    auto is_ndash_dimension = [](ComponentValue const& value) -> bool {
        if (!value.is(Token::Type::Dimension))
            return false;
        if (!value.token().number().is_integer())
            return false;
        if (!value.token().dimension_unit().equals_ignoring_ascii_case("n-"sv))
            return false;
        return true;
    };
    auto is_ndashdigit_dimension = [](ComponentValue const& value) -> bool {
        if (!value.is(Token::Type::Dimension))
            return false;
        if (!value.token().number().is_integer())
            return false;
        auto dimension_unit = value.token().dimension_unit();
        if (!dimension_unit.starts_with_bytes("n-"sv, CaseSensitivity::CaseInsensitive))
            return false;
        for (size_t i = 2; i < dimension_unit.bytes_as_string_view().length(); ++i) {
            if (!is_ascii_digit(dimension_unit.bytes_as_string_view()[i]))
                return false;
        }
        return true;
    };
    auto is_ndashdigit_ident = [](ComponentValue const& value) -> bool {
        if (!value.is(Token::Type::Ident))
            return false;
        auto ident = value.token().ident();
        if (!ident.starts_with_bytes("n-"sv, CaseSensitivity::CaseInsensitive))
            return false;
        for (size_t i = 2; i < ident.bytes_as_string_view().length(); ++i) {
            if (!is_ascii_digit(ident.bytes_as_string_view()[i]))
                return false;
        }
        return true;
    };
    auto is_dashndashdigit_ident = [](ComponentValue const& value) -> bool {
        if (!value.is(Token::Type::Ident))
            return false;
        auto ident = value.token().ident();
        if (!ident.starts_with_bytes("-n-"sv, CaseSensitivity::CaseInsensitive))
            return false;
        if (ident.bytes_as_string_view().length() == 3)
            return false;
        for (size_t i = 3; i < ident.bytes_as_string_view().length(); ++i) {
            if (!is_ascii_digit(ident.bytes_as_string_view()[i]))
                return false;
        }
        return true;
    };
    auto is_integer = [](ComponentValue const& value) -> bool {
        return value.is(Token::Type::Number) && value.token().number().is_integer();
    };
    auto is_signed_integer = [](ComponentValue const& value) -> bool {
        return value.is(Token::Type::Number) && value.token().number().is_integer_with_explicit_sign();
    };
    auto is_signless_integer = [](ComponentValue const& value) -> bool {
        return value.is(Token::Type::Number) && !value.token().number().is_integer_with_explicit_sign();
    };

    // https://www.w3.org/TR/css-syntax-3/#the-anb-type
    // Unfortunately these can't be in the same order as in the spec.

    values.discard_whitespace();
    auto const& first_value = values.consume_a_token();

    // odd | even
    if (first_value.is(Token::Type::Ident)) {
        auto ident = first_value.token().ident();
        if (ident.equals_ignoring_ascii_case("odd"sv)) {
            transaction.commit();
            return Selector::SimpleSelector::ANPlusBPattern { 2, 1 };
        }
        if (ident.equals_ignoring_ascii_case("even"sv)) {
            transaction.commit();
            return Selector::SimpleSelector::ANPlusBPattern { 2, 0 };
        }
    }
    // <integer>
    if (is_integer(first_value)) {
        int b = first_value.token().to_integer();
        transaction.commit();
        return Selector::SimpleSelector::ANPlusBPattern { 0, b };
    }
    // <n-dimension>
    // <n-dimension> <signed-integer>
    // <n-dimension> ['+' | '-'] <signless-integer>
    if (is_n_dimension(first_value)) {
        int a = first_value.token().dimension_value_int();
        values.discard_whitespace();

        // <n-dimension> <signed-integer>
        if (is_signed_integer(values.next_token())) {
            int b = values.consume_a_token().token().to_integer();
            transaction.commit();
            return Selector::SimpleSelector::ANPlusBPattern { a, b };
        }

        // <n-dimension> ['+' | '-'] <signless-integer>
        {
            auto child_transaction = transaction.create_child();
            auto const& second_value = values.consume_a_token();
            values.discard_whitespace();
            auto const& third_value = values.consume_a_token();

            if (is_sign(second_value) && is_signless_integer(third_value)) {
                int b = third_value.token().to_integer() * (second_value.is_delim('+') ? 1 : -1);
                child_transaction.commit();
                return Selector::SimpleSelector::ANPlusBPattern { a, b };
            }
        }

        // <n-dimension>
        transaction.commit();
        return Selector::SimpleSelector::ANPlusBPattern { a, 0 };
    }
    // <ndash-dimension> <signless-integer>
    if (is_ndash_dimension(first_value)) {
        values.discard_whitespace();
        auto const& second_value = values.consume_a_token();
        if (is_signless_integer(second_value)) {
            int a = first_value.token().dimension_value_int();
            int b = -second_value.token().to_integer();
            transaction.commit();
            return Selector::SimpleSelector::ANPlusBPattern { a, b };
        }

        return syntax_error();
    }
    // <ndashdigit-dimension>
    if (is_ndashdigit_dimension(first_value)) {
        auto const& dimension = first_value.token();
        int a = dimension.dimension_value_int();
        auto maybe_b = dimension.dimension_unit().bytes_as_string_view().substring_view(1).to_number<int>();
        if (maybe_b.has_value()) {
            transaction.commit();
            return Selector::SimpleSelector::ANPlusBPattern { a, maybe_b.value() };
        }

        return syntax_error();
    }
    // <dashndashdigit-ident>
    if (is_dashndashdigit_ident(first_value)) {
        auto maybe_b = first_value.token().ident().bytes_as_string_view().substring_view(2).to_number<int>();
        if (maybe_b.has_value()) {
            transaction.commit();
            return Selector::SimpleSelector::ANPlusBPattern { -1, maybe_b.value() };
        }

        return syntax_error();
    }
    // -n
    // -n <signed-integer>
    // -n ['+' | '-'] <signless-integer>
    if (first_value.is_ident("-n"sv)) {
        values.discard_whitespace();

        // -n <signed-integer>
        if (is_signed_integer(values.next_token())) {
            int b = values.consume_a_token().token().to_integer();
            transaction.commit();
            return Selector::SimpleSelector::ANPlusBPattern { -1, b };
        }

        // -n ['+' | '-'] <signless-integer>
        {
            auto child_transaction = transaction.create_child();
            auto const& second_value = values.consume_a_token();
            values.discard_whitespace();
            auto const& third_value = values.consume_a_token();

            if (is_sign(second_value) && is_signless_integer(third_value)) {
                int b = third_value.token().to_integer() * (second_value.is_delim('+') ? 1 : -1);
                child_transaction.commit();
                return Selector::SimpleSelector::ANPlusBPattern { -1, b };
            }
        }

        // -n
        transaction.commit();
        return Selector::SimpleSelector::ANPlusBPattern { -1, 0 };
    }
    // -n- <signless-integer>
    if (first_value.is_ident("-n-"sv)) {
        values.discard_whitespace();
        auto const& second_value = values.consume_a_token();
        if (is_signless_integer(second_value)) {
            int b = -second_value.token().to_integer();
            transaction.commit();
            return Selector::SimpleSelector::ANPlusBPattern { -1, b };
        }

        return syntax_error();
    }

    // All that's left now are these:
    // '+'?† n
    // '+'?† n <signed-integer>
    // '+'?† n ['+' | '-'] <signless-integer>
    // '+'?† n- <signless-integer>
    // '+'?† <ndashdigit-ident>
    // In all of these cases, the + is optional, and has no effect.
    // So, we just skip the +, and carry on.
    if (!first_value.is_delim('+')) {
        values.reconsume_current_input_token();
        // We do *not* skip whitespace here.
    }

    auto const& first_after_plus = values.consume_a_token();
    // '+'?† n
    // '+'?† n <signed-integer>
    // '+'?† n ['+' | '-'] <signless-integer>
    if (first_after_plus.is_ident("n"sv)) {
        values.discard_whitespace();

        // '+'?† n <signed-integer>
        if (is_signed_integer(values.next_token())) {
            int b = values.consume_a_token().token().to_integer();
            transaction.commit();
            return Selector::SimpleSelector::ANPlusBPattern { 1, b };
        }

        // '+'?† n ['+' | '-'] <signless-integer>
        {
            auto child_transaction = transaction.create_child();
            auto const& second_value = values.consume_a_token();
            values.discard_whitespace();
            auto const& third_value = values.consume_a_token();

            if (is_sign(second_value) && is_signless_integer(third_value)) {
                int b = third_value.token().to_integer() * (second_value.is_delim('+') ? 1 : -1);
                child_transaction.commit();
                return Selector::SimpleSelector::ANPlusBPattern { 1, b };
            }
        }

        // '+'?† n
        transaction.commit();
        return Selector::SimpleSelector::ANPlusBPattern { 1, 0 };
    }

    // '+'?† n- <signless-integer>
    if (first_after_plus.is_ident("n-"sv)) {
        values.discard_whitespace();
        auto const& second_value = values.consume_a_token();
        if (is_signless_integer(second_value)) {
            int b = -second_value.token().to_integer();
            transaction.commit();
            return Selector::SimpleSelector::ANPlusBPattern { 1, b };
        }

        return syntax_error();
    }

    // '+'?† <ndashdigit-ident>
    if (is_ndashdigit_ident(first_after_plus)) {
        auto maybe_b = first_after_plus.token().ident().bytes_as_string_view().substring_view(1).to_number<int>();
        if (maybe_b.has_value()) {
            transaction.commit();
            return Selector::SimpleSelector::ANPlusBPattern { 1, maybe_b.value() };
        }

        return syntax_error();
    }

    return syntax_error();
}

}
