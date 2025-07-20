/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/String.h>
#include <LibWeb/Export.h>

namespace Web::CSS::Parser {

struct UnknownPropertyError {
    FlyString rule_name { "style"_fly_string };
    FlyString property_name;
    bool operator==(UnknownPropertyError const&) const = default;
    unsigned hash() const
    {
        return pair_int_hash(rule_name.hash(), property_name.hash());
    }
};

struct UnknownRuleError {
    FlyString rule_name;
    bool operator==(UnknownRuleError const&) const = default;
    unsigned hash() const { return rule_name.hash(); }
};

struct UnknownMediaFeatureError {
    FlyString media_feature_name;
    bool operator==(UnknownMediaFeatureError const&) const = default;
    unsigned hash() const { return media_feature_name.hash(); }
};

struct UnknownPseudoClassOrElementError {
    FlyString rule_name { "style"_fly_string };
    FlyString name;
    bool operator==(UnknownPseudoClassOrElementError const&) const = default;
    unsigned hash() const
    {
        return pair_int_hash(rule_name.hash(), name.hash());
    }
};

struct InvalidPropertyError {
    FlyString rule_name { "style"_fly_string };
    FlyString property_name;
    String value_string;
    String description;
    bool operator==(InvalidPropertyError const&) const = default;
    unsigned hash() const
    {
        return pair_int_hash(rule_name.hash(), pair_int_hash(property_name.hash(), pair_int_hash(value_string.hash(), description.hash())));
    }
};

struct InvalidValueError {
    FlyString value_type;
    String value_string;
    String description;
    bool operator==(InvalidValueError const&) const = default;
    unsigned hash() const
    {
        return pair_int_hash(value_type.hash(), pair_int_hash(value_string.hash(), description.hash()));
    }
};

struct InvalidRuleError {
    FlyString rule_name;
    String prelude;
    String description;
    bool operator==(InvalidRuleError const&) const = default;
    unsigned hash() const
    {
        return pair_int_hash(rule_name.hash(), pair_int_hash(prelude.hash(), description.hash()));
    }
};

struct InvalidQueryError {
    FlyString query_type { "@media"_fly_string };
    String value_string;
    String description;
    bool operator==(InvalidQueryError const&) const = default;
    unsigned hash() const
    {
        return pair_int_hash(query_type.hash(), pair_int_hash(value_string.hash(), description.hash()));
    }
};

struct InvalidSelectorError {
    FlyString rule_name { "style"_fly_string };
    String value_string;
    String description;
    bool operator==(InvalidSelectorError const&) const = default;
    unsigned hash() const
    {
        return pair_int_hash(rule_name.hash(), pair_int_hash(value_string.hash(), description.hash()));
    }
};

struct InvalidPseudoClassOrElementError {
    FlyString name;
    String value_string;
    String description;
    bool operator==(InvalidPseudoClassOrElementError const&) const = default;
    unsigned hash() const
    {
        return pair_int_hash(name.hash(), pair_int_hash(value_string.hash(), description.hash()));
    }
};

struct InvalidRuleLocationError {
    FlyString outer_rule_name;
    FlyString inner_rule_name;
    bool operator==(InvalidRuleLocationError const&) const = default;
    unsigned hash() const
    {
        return pair_int_hash(outer_rule_name.hash(), inner_rule_name.hash());
    }
};

using ParsingError = Variant<UnknownPropertyError, UnknownRuleError, UnknownMediaFeatureError, UnknownPseudoClassOrElementError, InvalidPropertyError, InvalidValueError, InvalidRuleError, InvalidQueryError, InvalidSelectorError, InvalidPseudoClassOrElementError, InvalidRuleLocationError>;

String serialize_parsing_error(ParsingError const&);

class WEB_API ErrorReporter {
public:
    static ErrorReporter& the();

    void report(ParsingError&&);
    void dump() const;

private:
    explicit ErrorReporter() = default;
    struct Metadata {
        u32 occurrences { 1 };
    };
    HashMap<ParsingError, Metadata> m_errors;
};

}

template<>
struct AK::Traits<Web::CSS::Parser::ParsingError> : public DefaultTraits<Web::CSS::Parser::ParsingError> {
    static unsigned hash(Web::CSS::Parser::ParsingError const& error)
    {
        return error.visit([](auto const& it) { return it.hash(); });
    }
};
