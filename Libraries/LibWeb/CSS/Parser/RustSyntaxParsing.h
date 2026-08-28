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
#include <LibWeb/CSS/Descriptor.h>
#include <LibWeb/CSS/DescriptorNameAndID.h>
#include <LibWeb/CSS/PageSelector.h>
#include <LibWeb/CSS/Parser/RuleContext.h>
#include <LibWeb/CSS/Parser/RustSyntaxHandle.h>
#include <LibWeb/CSS/RustQueryHandle.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/Forward.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

class Parser;

inline ValueParserFFI::FfiUtf16View ffi_utf16_view(Utf16View view)
{
    return {
        .ascii = view.has_ascii_storage() ? reinterpret_cast<u8 const*>(view.ascii_span().data()) : nullptr,
        .utf16 = view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(view.utf16_span().data()),
        .length = view.length_in_code_units(),
    };
}

PageSelectorList page_selector_list_from_rust(ValueParserFFI::FfiPageSelectorListData const&);

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
    MediaQueries,
    SupportsCondition,
    ContainerConditions,
    Property,
    FontFeatureValuesRule,
};

struct ParsedRulePreludeItem {
    Optional<Utf16FlyString> value;
    Optional<SelectorList> selectors;
    Optional<RustQueryHandle> query;
    Optional<RustSyntaxHandle> syntax;
    RefPtr<StyleValue const> style_value;
    double number_value { 0 };
    u8 kind { 0 };
};

struct ParsedRulePrelude {
    ParsedRulePreludeKind kind { ParsedRulePreludeKind::Unparsed };
    Optional<Utf16FlyString> name;
    Optional<Utf16FlyString> secondary;
    Optional<RustSyntaxHandle> syntax;
    Vector<ParsedRulePreludeItem> items;
    PageSelectorList page_selectors;
};

struct AtRule {
    ValueParserFFI::FfiRuleKind kind;
    Utf16FlyString name;
    ParsedRulePrelude parsed_prelude;
    Vector<Descriptor> descriptors;
    Vector<RuleOrListOfDeclarations> child_rules_and_lists_of_declarations;
    bool is_block_rule { false };

    void for_each_as_declaration_list(DeclarationVisitor&& visit) const;
    void for_each_as_qualified_rule_list(QualifiedRuleVisitor&& visit) const;
    void for_each_as_declaration_rule_list(AtRuleVisitor&& visit_at_rule, DeclarationVisitor&& visit_declaration) const;
};

struct QualifiedRule {
    ValueParserFFI::FfiRuleKind kind;
    Optional<SelectorList> selectors;
    ParsedRulePrelude parsed_prelude;
    Vector<Declaration> declarations;
    Vector<RuleOrListOfDeclarations> child_rules;
    Optional<SourcePosition> source_position = {};

    void for_each_as_declaration_list(DeclarationVisitor&& visit) const;
};

struct Declaration {
    Optional<Utf16FlyString> name;
    Important important = Important::No;
    Optional<SourcePosition> source_position = {};
    Optional<Utf16String> value_text;
    Optional<PropertyID> parsed_property_id;
    Optional<StylePropertyAndName> property;
    ValueParserFFI::FfiDeclarationRejection rejection { ValueParserFFI::FfiDeclarationRejection::None };
    Optional<DescriptorNameAndID> descriptor_name_and_id;
    RefPtr<StyleValue const> parsed_value;
    Optional<Vector<u32>> font_feature_values;
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
