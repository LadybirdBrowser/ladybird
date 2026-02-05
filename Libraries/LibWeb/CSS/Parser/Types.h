/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Parser/Token.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

// https://drafts.csswg.org/css-syntax/#css-rule
using Rule = Variant<AtRule, QualifiedRule>;

using RuleOrListOfDeclarations = Variant<Rule, Vector<Declaration, 0>>;

using AtRuleVisitor = AK::Function<void(AtRule const&)>;
using QualifiedRuleVisitor = AK::Function<void(QualifiedRule const&)>;
using RuleVisitor = AK::Function<void(Rule const&)>;
using DeclarationVisitor = AK::Function<void(Declaration const&)>;

// https://drafts.csswg.org/css-syntax/#ref-for-at-rule%E2%91%A0%E2%91%A1
struct AtRule {
    FlyString name;
    Vector<ComponentValue> prelude;
    Vector<RuleOrListOfDeclarations> child_rules_and_lists_of_declarations;
    bool is_block_rule { false };

    void for_each(AtRuleVisitor&& visit_at_rule, QualifiedRuleVisitor&& visit_qualified_rule, DeclarationVisitor&& visit_declaration) const;
    void for_each_as_declaration_list(DeclarationVisitor&& visit) const;
    void for_each_as_qualified_rule_list(QualifiedRuleVisitor&& visit) const;
    void for_each_as_at_rule_list(AtRuleVisitor&& visit) const;
    void for_each_as_declaration_rule_list(AtRuleVisitor&& visit_at_rule, DeclarationVisitor&& visit_declaration) const;
    void for_each_as_rule_list(RuleVisitor&& visit) const;
};

// https://drafts.csswg.org/css-syntax/#qualified-rule
struct QualifiedRule {
    Vector<ComponentValue> prelude;
    Vector<Declaration> declarations;
    Vector<RuleOrListOfDeclarations> child_rules;

    void for_each_as_declaration_list(FlyString const& rule_name, DeclarationVisitor&& visit) const;
};

// https://drafts.csswg.org/css-syntax/#declaration
struct Declaration {
    FlyString name;
    Vector<ComponentValue> value;
    Important important = Important::No;
    Optional<String> original_value_text = {};
    Optional<String> original_full_text = {};
};

struct SubstitutionFunctionsPresence {
    bool attr { false };
    bool env { false };
    bool var { false };

    bool has_any() const { return attr || env || var; }
};

// https://drafts.csswg.org/css-syntax/#simple-block
struct SimpleBlock {
    Token token;
    Vector<ComponentValue> value;
    Token end_token = {};

    bool is_curly() const { return token.is(Token::Type::OpenCurly); }
    bool is_paren() const { return token.is(Token::Type::OpenParen); }
    bool is_square() const { return token.is(Token::Type::OpenSquare); }

    String to_string() const;
    String original_source_text() const;
    void contains_arbitrary_substitution_function(SubstitutionFunctionsPresence&) const;

    bool operator==(SimpleBlock const& other) const { return token == other.token && value == other.value; }
};

// https://drafts.csswg.org/css-syntax/#function
struct Function {
    FlyString name;
    Vector<ComponentValue> value;
    Token name_token = {};
    Token end_token = {};

    String to_string() const;
    String original_source_text() const;
    void contains_arbitrary_substitution_function(SubstitutionFunctionsPresence&) const;

    bool operator==(Function const& other) const { return name == other.name && value == other.value; }
};

// https://drafts.csswg.org/css-variables/#guaranteed-invalid-value
struct GuaranteedInvalidValue {
    GuaranteedInvalidValue() = default;
    String to_string() const { return {}; }
    String original_source_text() const { return {}; }

    bool operator==(GuaranteedInvalidValue const&) const = default;
};

}
