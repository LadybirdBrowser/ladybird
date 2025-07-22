/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>

namespace Web::CSS::Parser {

String serialize_parsing_error(ParsingError const& error)
{
    return error.visit(
        [](UnknownPropertyError const& error) {
            return MUST(String::formatted("Unknown property '{}' in {} rule.", error.property_name, error.rule_name));
        },
        [](UnknownRuleError const& error) {
            return MUST(String::formatted("Unknown rule '{}'.", error.rule_name));
        },
        [](UnknownMediaFeatureError const& error) {
            return MUST(String::formatted("Unknown media feature '{}'.", error.media_feature_name));
        },
        [](UnknownPseudoClassOrElementError const& error) {
            return MUST(String::formatted("Unknown pseudo class or element '{}' in {} selector.", error.name, error.rule_name));
        },
        [](InvalidPropertyError const& error) {
            return MUST(String::formatted("Property '{}' in {} rule has invalid value `{}`.", error.property_name, error.rule_name, error.value_string));
        },
        [](InvalidValueError const& error) {
            return MUST(String::formatted("Unable to parse {} from `{}`: {}", error.value_type, error.value_string, error.description));
        },
        [](InvalidRuleError const& error) {
            return MUST(String::formatted("'{}' rule with prelude `{}` is invalid: {}", error.rule_name, error.prelude, error.description));
        },
        [](InvalidQueryError const& error) {
            return MUST(String::formatted("'{}' query `{}` is invalid: {}", error.query_type, error.value_string, error.description));
        },
        [](InvalidSelectorError const& error) {
            return MUST(String::formatted("{} selector `{}` is invalid: {}", error.rule_name, error.value_string, error.description));
        },
        [](InvalidPseudoClassOrElementError const& error) {
            return MUST(String::formatted("Pseudo '{}' value `{}` is invalid: {}", error.name, error.value_string, error.description));
        },
        [](InvalidRuleLocationError const& error) {
            return MUST(String::formatted("'{}' rule is invalid inside {}", error.inner_rule_name, error.outer_rule_name));
        });
}

ErrorReporter& ErrorReporter::the()
{
    static ErrorReporter s_error_reporter {};
    return s_error_reporter;
}

void ErrorReporter::report(ParsingError&& error)
{
    if (auto existing = m_errors.get(error); existing.has_value()) {
        existing->occurrences++;
        return;
    }

    dbgln_if(CSS_PARSER_DEBUG, "CSS parsing error: {}", serialize_parsing_error(error));
    m_errors.set(move(error), { .occurrences = 1 });
}

}
