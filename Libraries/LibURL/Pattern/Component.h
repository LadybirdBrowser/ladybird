/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <LibRegex/Regex.h>
#include <LibURL/Pattern/PatternParser.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#component
struct Component {
    static PatternErrorOr<Component> compile(Utf8View const& input, PatternParser::EncodingCallback, Options const&);

    // https://urlpattern.spec.whatwg.org/#dictdef-urlpatterncomponentresult
    struct Result {
        String input;
        OrderedHashMap<String, Variant<String, Empty>> groups;
    };

    Result create_match_result(String const& input, regex::RegexResult const& exec_result) const;

    // https://urlpattern.spec.whatwg.org/#component-pattern-string
    // pattern string, a well formed pattern string
    String pattern_string;

    // https://urlpattern.spec.whatwg.org/#component-regular-expression
    // regular expression, a RegExp
    OwnPtr<Regex<ECMA262>> regular_expression;

    // https://urlpattern.spec.whatwg.org/#component-group-name-list
    // group name list, a list of strings
    Vector<String> group_name_list;

    // https://urlpattern.spec.whatwg.org/#component-has-regexp-groups
    // has regexp groups, a boolean
    bool has_regexp_groups {};
};

bool protocol_component_matches_a_special_scheme(Component const& protocol_component);

}
