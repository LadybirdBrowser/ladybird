/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>
#include <AK/Utf16FlyString.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Parser/RuleContext.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/Forward.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

class Parser;

using Rule = Variant<AtRule, QualifiedRule>;
using RuleOrListOfDeclarations = Variant<Rule, Vector<Declaration, 0>>;

using AtRuleVisitor = AK::Function<void(AtRule const&)>;
using QualifiedRuleVisitor = AK::Function<void(QualifiedRule const&)>;
using RuleVisitor = AK::Function<void(Rule const&)>;
using DeclarationVisitor = AK::Function<void(Declaration const&)>;

enum class ParsedRulePreludeKind : u8 {
    Unparsed,
    Invalid,
    Empty,
    Name,
    Names,
    KeyframeSelectors,
    Namespace,
    PageSelectors,
    FontFamilyNames,
    Scope,
    Import,
    Function,
};

struct ParsedRulePreludeItem {
    Optional<Utf16FlyString> value;
    double number_value { 0 };
    u8 flags { 0 };
};

struct ParsedRulePrelude {
    ParsedRulePreludeKind kind { ParsedRulePreludeKind::Unparsed };
    Optional<Utf16FlyString> name;
    Optional<Utf16FlyString> secondary;
    Vector<ParsedRulePreludeItem> items;
};

struct AtRule {
    Utf16FlyString name;
    Utf16String prelude_text;
    ParsedRulePrelude parsed_prelude;
    Vector<RuleOrListOfDeclarations> child_rules_and_lists_of_declarations;
    bool is_block_rule { false };

    void for_each(AtRuleVisitor&& visit_at_rule, QualifiedRuleVisitor&& visit_qualified_rule, DeclarationVisitor&& visit_declaration) const;
    void for_each_as_declaration_list(DeclarationVisitor&& visit) const;
    void for_each_as_qualified_rule_list(QualifiedRuleVisitor&& visit) const;
    void for_each_as_declaration_rule_list(AtRuleVisitor&& visit_at_rule, DeclarationVisitor&& visit_declaration) const;
};

struct QualifiedRule {
    Utf16String prelude_text;
    ParsedRulePrelude parsed_prelude;
    Vector<Declaration> declarations;
    Vector<RuleOrListOfDeclarations> child_rules;
    Optional<SourcePosition> source_position = {};

    void for_each_as_declaration_list(Utf16FlyString const& rule_name, DeclarationVisitor&& visit) const;
};

struct Declaration {
    Utf16FlyString name;
    Important important = Important::No;
    Optional<Utf16String> original_value_text = {};
    Optional<Utf16String> original_full_text = {};
    Optional<SourcePosition> source_position = {};
    Utf16String value_text;
    Optional<PropertyID> parsed_property_id;
    RefPtr<StyleValue const> parsed_value;
};

enum class PreservePropertySourceText {
    No,
    Yes,
};

enum class RuleNesting {
    No,
    Yes,
};

class RustSyntaxParser {
public:
    static Optional<Rule> parse_rule(Parser&, ReadonlySpan<RuleContext>, RuleNesting);
    static ParsedRulePrelude parse_keyframe_selectors(Parser&);
    static Vector<Rule> parse_stylesheet(Parser&);
    static Vector<RuleOrListOfDeclarations> parse_block_contents(Parser&, ReadonlySpan<RuleContext>, PreservePropertySourceText = PreservePropertySourceText::No);
    static Vector<RuleOrListOfDeclarations> parse_block_contents(Parser&, Utf16View, ReadonlySpan<RuleContext>, PreservePropertySourceText = PreservePropertySourceText::No);
    static RefPtr<StyleValue const> parse_descriptor(Parser&, AtRuleID, DescriptorNameAndID const&);
};

}
