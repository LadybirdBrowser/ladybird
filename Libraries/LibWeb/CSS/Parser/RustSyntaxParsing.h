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
#include <LibWeb/CSS/RustDeclarationBlock.h>
#include <LibWeb/CSS/RustQueryHandle.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/Forward.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

class Parser;
struct ParsingParams;

// A uniquely owned Rust parse result can be transferred from a worker to the document thread.
class RustStyleSheetParse {
    AK_MAKE_NONCOPYABLE(RustStyleSheetParse);

public:
    explicit RustStyleSheetParse(ValueParserFFI::FfiSyntaxParse* parse)
        : m_parse(parse)
    {
        VERIFY(m_parse);
    }
    RustStyleSheetParse(RustStyleSheetParse&& other)
        : m_parse(exchange(other.m_parse, nullptr))
    {
    }
    ~RustStyleSheetParse()
    {
        if (m_parse)
            ValueParserFFI::rust_css_syntax_parse_free(m_parse);
    }

    ValueParserFFI::FfiSyntaxParseData data() const { return ValueParserFFI::rust_css_syntax_parse_data(m_parse); }
    RustStyleSheetParse share() const { return RustStyleSheetParse { ValueParserFFI::rust_css_syntax_parse_share(m_parse) }; }
    // Retain the same consumer and its borrowed views on the document thread.
    RustStyleSheetParse retain() const { return RustStyleSheetParse { ValueParserFFI::rust_css_syntax_parse_retain(m_parse) }; }

private:
    ValueParserFFI::FfiSyntaxParse* m_parse;
};

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
class DeclarationList {
public:
    DeclarationList(RustStyleSheetParse const&, ValueParserFFI::FfiSyntaxItem const&);
    Vector<Declaration> const& declarations() const;
    RustDeclarationBlock const& properties() const { return m_properties; }
    Optional<SourcePosition> source_position() const;

private:
    RustStyleSheetParse m_parse;
    RustDeclarationBlock m_properties;
    size_t m_start;
    size_t m_count;
    mutable Optional<Vector<Declaration>> m_declarations;
};

using RuleOrListOfDeclarations = Variant<Rule, DeclarationList>;

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
    Optional<RustDeclarationBlock> declarations;
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
    RustDeclarationBlock declarations;
    Vector<RuleOrListOfDeclarations> child_rules;
    Optional<SourcePosition> source_position = {};
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
    static RustStyleSheetParse parse_stylesheet(Parser&);
    static void parse_stylesheet_off_thread(ParsingParams const&, Utf16String, Function<void(RustStyleSheetParse)>);
    static Vector<Rule> stylesheet_rules(RustStyleSheetParse const&);
    static RustDeclarationBlock parse_declaration_block(Parser&, ReadonlySpan<RuleContext>);
    static Vector<RuleOrListOfDeclarations> parse_block_contents(Parser&, ReadonlySpan<RuleContext>, PreservePropertySourceText = PreservePropertySourceText::No);
    static Vector<RuleOrListOfDeclarations> parse_block_contents(Parser&, Utf16View, ReadonlySpan<RuleContext>, PreservePropertySourceText = PreservePropertySourceText::No);
    static RefPtr<StyleValue const> parse_descriptor(Parser&, AtRuleID, DescriptorNameAndID const&);
};

}
