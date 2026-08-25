/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/Utf16View.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustQueryParsing.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

void AtRule::for_each(AtRuleVisitor&& visit_at_rule, QualifiedRuleVisitor&& visit_qualified_rule, DeclarationVisitor&& visit_declaration) const
{
    for (auto const& child : child_rules_and_lists_of_declarations) {
        child.visit(
            [&](Rule const& rule) {
                rule.visit(
                    [&](AtRule const& at_rule) { visit_at_rule(at_rule); },
                    [&](QualifiedRule const& qualified_rule) { visit_qualified_rule(qualified_rule); });
            },
            [&](Vector<Declaration> const& declarations) {
                for (auto const& declaration : declarations)
                    visit_declaration(declaration);
            });
    }
}

void AtRule::for_each_as_declaration_list(DeclarationVisitor&& visit) const
{
    for_each(
        [this](auto const& at_rule) {
            ErrorReporter::the().report(InvalidRuleLocationError {
                .outer_rule_name = Utf16String::formatted("@{}", name),
                .inner_rule_name = Utf16String::formatted("@{}", at_rule.name),
            });
        },
        [this](auto const&) {
            ErrorReporter::the().report(InvalidRuleLocationError {
                .outer_rule_name = Utf16String::formatted("@{}", name),
                .inner_rule_name = "qualified-rule"_utf16_fly_string,
            });
        },
        move(visit));
}

void AtRule::for_each_as_qualified_rule_list(QualifiedRuleVisitor&& visit) const
{
    for_each(
        [this](auto const& at_rule) {
            ErrorReporter::the().report(InvalidRuleLocationError {
                .outer_rule_name = Utf16String::formatted("@{}", name),
                .inner_rule_name = Utf16String::formatted("@{}", at_rule.name),
            });
        },
        move(visit),
        [this](auto const&) {
            ErrorReporter::the().report(InvalidRuleLocationError {
                .outer_rule_name = Utf16String::formatted("@{}", name),
                .inner_rule_name = "list-of-declarations"_utf16_fly_string,
            });
        });
}

void AtRule::for_each_as_declaration_rule_list(AtRuleVisitor&& visit_at_rule, DeclarationVisitor&& visit_declaration) const
{
    for_each(
        move(visit_at_rule),
        [this](auto const&) {
            ErrorReporter::the().report(InvalidRuleLocationError {
                .outer_rule_name = Utf16String::formatted("@{}", name),
                .inner_rule_name = "qualified-rule"_utf16_fly_string,
            });
        },
        move(visit_declaration));
}

void QualifiedRule::for_each_as_declaration_list(Utf16FlyString const& rule_name, DeclarationVisitor&& visit) const
{
    for (auto const& declaration : declarations)
        visit(declaration);

    for (auto const& child : child_rules) {
        child.visit(
            [&](Rule const&) {
                ErrorReporter::the().report(InvalidRuleLocationError {
                    .outer_rule_name = rule_name,
                    .inner_rule_name = "qualified-rule"_utf16_fly_string,
                });
            },
            [&](Vector<Declaration> const& declarations) {
                for (auto const& declaration : declarations)
                    visit(declaration);
            });
    }
}

using namespace ValueParserFFI;

static u16 resolve_property_id(u16 const* code_units, size_t length)
{
    auto name = Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(code_units), length });
    auto property = PropertyNameAndID::from_name(move(name));
    return property.has_value() ? to_underlying(property->id()) : NumericLimits<u16>::max();
}

static Utf16View utf16_value(FfiSyntaxParseData const& data, size_t offset, size_t length)
{
    VERIFY(offset <= data.value_count);
    VERIFY(length <= data.value_count - offset);
    return { reinterpret_cast<char16_t const*>(data.values + offset), length };
}

static SourcePosition source_position(size_t line, size_t column)
{
    VERIFY(line <= NumericLimits<u32>::max());
    VERIFY(column <= NumericLimits<u32>::max());
    return { static_cast<u32>(line), static_cast<u32>(column) };
}

static void report_diagnostics(FfiSyntaxParseData const& data)
{
    for (size_t index = 0; index < data.diagnostic_count; ++index) {
        auto const& diagnostic = data.diagnostics[index];
        VERIFY(diagnostic.code <= to_underlying(SyntaxDiagnosticCode::BadUrl));
        auto start = source_position(diagnostic.start_line, diagnostic.start_column);
        auto end = source_position(diagnostic.end_line, diagnostic.end_column);
        ErrorReporter::the().report(SyntaxDiagnosticError {
            .code = static_cast<SyntaxDiagnosticCode>(diagnostic.code),
            .start_line = start.line,
            .start_column = start.column,
            .end_line = end.line,
            .end_column = end.column,
        });
    }
}

static Declaration declaration(FfiSyntaxParseData const& data, size_t index)
{
    VERIFY(index < data.declaration_count);
    auto const& declaration = data.declarations[index];
    auto optional_text = [&](size_t offset, size_t length) -> Optional<Utf16String> {
        if (offset == NumericLimits<size_t>::max())
            return {};
        return Utf16String::from_utf16(utf16_value(data, offset, length));
    };
    Optional<Utf16String> original_value_text;
    if (declaration.original_value_offset != NumericLimits<size_t>::max())
        original_value_text = Utf16String::from_utf16(utf16_value(data, declaration.original_value_offset, declaration.original_value_length));
    auto value_text = Utf16String::from_utf16(utf16_value(data, declaration.value_source_offset, declaration.value_source_length));
    Optional<PropertyID> parsed_property_id;
    Optional<DescriptorNameAndID> descriptor_name_and_id;
    RefPtr<StyleValue const> parsed_value;
    if (declaration.is_property) {
        if (declaration.property_id != NumericLimits<u16>::max())
            parsed_property_id = static_cast<PropertyID>(declaration.property_id);
        VERIFY(declaration.descriptor_id == NumericLimits<u8>::max());
    } else if (declaration.descriptor_id != NumericLimits<u8>::max()) {
        VERIFY(declaration.descriptor_id <= to_underlying(DescriptorID::Custom));
        auto descriptor_id = static_cast<DescriptorID>(declaration.descriptor_id);
        descriptor_name_and_id = descriptor_id == DescriptorID::Custom
            ? DescriptorNameAndID::from_custom_name(Utf16FlyString::from_utf16(utf16_value(data, declaration.name_offset, declaration.name_length)))
            : DescriptorNameAndID::from_id(descriptor_id);
    }
    if (declaration.parse_status == FfiParseStatus::Parsed) {
        VERIFY(declaration.parsed_value);
        parsed_value = StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(declaration.parsed_value));
    } else {
        VERIFY(!declaration.parsed_value);
    }
    Optional<Vector<u32>> font_feature_values;
    if (declaration.font_feature_values_start != NumericLimits<size_t>::max()) {
        VERIFY(declaration.font_feature_values_start <= data.font_feature_value_count);
        VERIFY(declaration.font_feature_value_count <= data.font_feature_value_count - declaration.font_feature_values_start);
        font_feature_values = Vector<u32> {};
        font_feature_values->ensure_capacity(declaration.font_feature_value_count);
        for (size_t index = 0; index < declaration.font_feature_value_count; ++index)
            font_feature_values->unchecked_append(data.font_feature_values[declaration.font_feature_values_start + index]);
    }
    return Declaration {
        .name = Utf16FlyString::from_utf16(utf16_value(data, declaration.name_offset, declaration.name_length)),
        .important = declaration.important ? Important::Yes : Important::No,
        .original_value_text = move(original_value_text),
        .original_full_text = optional_text(declaration.original_full_text_offset, declaration.original_full_text_length),
        .source_position = source_position(declaration.start_line, declaration.start_column),
        .value_text = move(value_text),
        .parsed_property_id = parsed_property_id,
        .descriptor_name_and_id = move(descriptor_name_and_id),
        .parsed_value = move(parsed_value),
        .font_feature_values = move(font_feature_values),
    };
}

static Vector<Descriptor> descriptors(FfiSyntaxParseData const& data, size_t start, size_t count)
{
    VERIFY(start <= data.descriptor_count);
    VERIFY(count <= data.descriptor_count - start);
    Vector<Descriptor> result;
    result.ensure_capacity(count);
    for (size_t index = 0; index < count; ++index) {
        auto const& descriptor = data.descriptors[start + index];
        VERIFY(descriptor.descriptor_id <= to_underlying(DescriptorID::Custom));
        VERIFY(descriptor.value);
        auto descriptor_id = static_cast<DescriptorID>(descriptor.descriptor_id);
        auto name_and_id = descriptor_id == DescriptorID::Custom
            ? DescriptorNameAndID::from_custom_name(Utf16FlyString::from_utf16(utf16_value(data, descriptor.name_offset, descriptor.name_length)))
            : DescriptorNameAndID::from_id(descriptor_id);
        result.unchecked_append({
            .descriptor_name_and_id = move(name_and_id),
            .value = StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(descriptor.value)),
        });
    }
    return result;
}

static Vector<Declaration> declarations(FfiSyntaxParseData const& data, size_t start, size_t count)
{
    VERIFY(start <= data.declaration_count);
    VERIFY(count <= data.declaration_count - start);
    Vector<Declaration> result;
    result.ensure_capacity(count);
    for (size_t index = 0; index < count; ++index)
        result.unchecked_append(declaration(data, start + index));
    return result;
}

static ParsedRulePrelude parsed_rule_prelude(FfiSyntaxParseData const& data, FfiSyntaxRule const& rule)
{
    VERIFY(rule.parsed_prelude_kind <= to_underlying(ParsedRulePreludeKind::FontFeatureValuesRule));
    VERIFY(rule.parsed_prelude_items_start <= data.prelude_item_count);
    VERIFY(rule.parsed_prelude_item_count <= data.prelude_item_count - rule.parsed_prelude_items_start);
    auto optional_string = [&](size_t offset, size_t length) -> Optional<Utf16FlyString> {
        if (offset == NumericLimits<size_t>::max())
            return {};
        return Utf16FlyString::from_utf16(utf16_value(data, offset, length));
    };
    Vector<ParsedRulePreludeItem> items;
    items.ensure_capacity(rule.parsed_prelude_item_count);
    for (size_t index = 0; index < rule.parsed_prelude_item_count; ++index) {
        auto const& item = data.prelude_items[rule.parsed_prelude_items_start + index];
        Optional<SelectorList> selectors;
        if (item.selector_list)
            selectors = selector_list_from_rust(static_cast<SelectorFFI::RustParsedSelectorList*>(item.selector_list));
        Optional<RustQueryHandle> query;
        if (item.query)
            query = RustQueryHandle::retained(static_cast<FfiQueryHandle const*>(item.query));
        Optional<RustSyntaxHandle> syntax;
        if (item.syntax)
            syntax = RustSyntaxHandle { item.syntax };
        RefPtr<StyleValue const> style_value;
        if (item.style_value)
            style_value = StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(item.style_value));
        items.unchecked_append({
            .value = optional_string(item.value_offset, item.value_length),
            .selectors = move(selectors),
            .query = move(query),
            .syntax = move(syntax),
            .style_value = move(style_value),
            .number_value = item.number_value,
            .kind = item.kind,
        });
    }
    return {
        .kind = static_cast<ParsedRulePreludeKind>(rule.parsed_prelude_kind),
        .name = optional_string(rule.parsed_prelude_name_offset, rule.parsed_prelude_name_length),
        .secondary = optional_string(rule.parsed_prelude_secondary_offset, rule.parsed_prelude_secondary_length),
        .syntax = rule.parsed_prelude_syntax ? Optional<RustSyntaxHandle> { RustSyntaxHandle { rule.parsed_prelude_syntax } } : OptionalNone {},
        .items = move(items),
    };
}

static Rule rule(FfiSyntaxParseData const&, size_t);

static Vector<RuleOrListOfDeclarations> items(FfiSyntaxParseData const& data, size_t start, size_t count)
{
    VERIFY(start <= data.item_index_count);
    VERIFY(count <= data.item_index_count - start);
    Vector<RuleOrListOfDeclarations> result;
    result.ensure_capacity(count);
    for (size_t index = 0; index < count; ++index) {
        auto item_index = data.item_indices[start + index];
        VERIFY(item_index < data.item_count);
        auto const& item = data.items[item_index];
        if (item.item_type == 0)
            result.unchecked_append(rule(data, item.start));
        else {
            VERIFY(item.item_type == 1);
            result.unchecked_append(declarations(data, item.start, item.count));
        }
    }
    return result;
}

static Rule rule(FfiSyntaxParseData const& data, size_t index)
{
    VERIFY(index < data.rule_count);
    auto const& rule = data.rules[index];
    auto prelude_text = Utf16String::from_utf16(utf16_value(data, rule.prelude_source_offset, rule.prelude_source_length));
    auto children = items(data, rule.children_start, rule.child_count);
    if (rule.rule_type == 0) {
        return AtRule {
            .kind = rule.rule_kind,
            .name = Utf16FlyString::from_utf16(utf16_value(data, rule.name_offset, rule.name_length)),
            .prelude_text = move(prelude_text),
            .parsed_prelude = parsed_rule_prelude(data, rule),
            .descriptors = descriptors(data, rule.descriptors_start, rule.descriptor_count),
            .child_rules_and_lists_of_declarations = move(children),
            .is_block_rule = rule.has_block,
        };
    }

    VERIFY(rule.rule_type == 1);
    Optional<SelectorList> selectors;
    if (rule.selector_list)
        selectors = selector_list_from_rust(static_cast<SelectorFFI::RustParsedSelectorList*>(rule.selector_list));
    return QualifiedRule {
        .prelude_text = move(prelude_text),
        .selectors = move(selectors),
        .parsed_prelude = parsed_rule_prelude(data, rule),
        .declarations = declarations(data, rule.declarations_start, rule.declaration_count),
        .child_rules = move(children),
        .source_position = rule.has_source_position ? Optional<SourcePosition> { source_position(rule.start_line, rule.start_column) } : OptionalNone {},
    };
}

Vector<Rule> RustSyntaxParser::parse_stylesheet(Parser& parser)
{
    auto context = parser.make_parse_context(Parser::ParseContextMode::Syntax);
    auto* parse = rust_parse_css_stylesheet_syntax(ffi_utf16_view(parser.m_source), &context.context, resolve_property_id, RustQueryParser::resolve_query_feature);
    VERIFY(parse);
    ScopeGuard free_parse = [&] { rust_css_syntax_parse_free(parse); };
    auto data = rust_css_syntax_parse_data(parse);
    report_diagnostics(data);
    Vector<Rule> result;
    result.ensure_capacity(data.root_count);
    for (size_t index = 0; index < data.root_count; ++index)
        result.unchecked_append(rule(data, data.roots[index]));
    return result;
}

Optional<Rule> RustSyntaxParser::parse_rule(Parser& parser, ReadonlySpan<RuleContext> contexts, RuleNesting nested)
{
    auto context = parser.make_parse_context(Parser::ParseContextMode::Syntax);
    static_assert(sizeof(RuleContext) == sizeof(u8));
    auto* parse = rust_parse_css_rule_syntax(ffi_utf16_view(parser.m_source), reinterpret_cast<u8 const*>(contexts.data()), contexts.size(), nested == RuleNesting::Yes, &context.context, resolve_property_id, RustQueryParser::resolve_query_feature);
    VERIFY(parse);
    ScopeGuard free_parse = [&] { rust_css_syntax_parse_free(parse); };
    auto data = rust_css_syntax_parse_data(parse);
    report_diagnostics(data);
    if (data.root_count != 1)
        return {};
    return rule(data, data.roots[0]);
}

ParsedRulePrelude RustSyntaxParser::parse_keyframe_selectors(Parser& parser)
{
    auto context = parser.make_parse_context(Parser::ParseContextMode::Syntax);
    auto* parse = rust_parse_css_keyframe_selectors_syntax(ffi_utf16_view(parser.m_source), &context.context, resolve_property_id, RustQueryParser::resolve_query_feature);
    VERIFY(parse);
    ScopeGuard free_parse = [&] { rust_css_syntax_parse_free(parse); };
    auto data = rust_css_syntax_parse_data(parse);
    report_diagnostics(data);
    VERIFY(data.root_count == 1);
    auto rule_index = data.roots[0];
    VERIFY(rule_index < data.rule_count);
    return parsed_rule_prelude(data, data.rules[rule_index]);
}

Vector<RuleOrListOfDeclarations> RustSyntaxParser::parse_block_contents(Parser& parser, ReadonlySpan<RuleContext> contexts, PreservePropertySourceText preserve_property_source_text)
{
    return parse_block_contents(parser, parser.m_source, contexts, preserve_property_source_text);
}

Vector<RuleOrListOfDeclarations> RustSyntaxParser::parse_block_contents(Parser& parser, Utf16View source, ReadonlySpan<RuleContext> contexts, PreservePropertySourceText preserve_property_source_text)
{
    static_assert(sizeof(RuleContext) == sizeof(u8));
    auto context = parser.make_parse_context(Parser::ParseContextMode::Syntax);
    auto* parse = rust_parse_css_block_syntax(ffi_utf16_view(source), reinterpret_cast<u8 const*>(contexts.data()), contexts.size(), &context.context, resolve_property_id, RustQueryParser::resolve_query_feature, preserve_property_source_text == PreservePropertySourceText::Yes);
    VERIFY(parse);
    ScopeGuard free_parse = [&] { rust_css_syntax_parse_free(parse); };
    auto data = rust_css_syntax_parse_data(parse);
    report_diagnostics(data);
    Vector<RuleOrListOfDeclarations> result;
    result.ensure_capacity(data.root_count);
    for (size_t index = 0; index < data.root_count; ++index) {
        auto item_index = data.roots[index];
        VERIFY(item_index < data.item_count);
        auto const& item = data.items[item_index];
        if (item.item_type == 0)
            result.unchecked_append(rule(data, item.start));
        else
            result.unchecked_append(declarations(data, item.start, item.count));
    }
    return result;
}

RefPtr<StyleValue const> RustSyntaxParser::parse_descriptor(Parser& parser, AtRuleID at_rule_id, DescriptorNameAndID const& descriptor)
{
    auto context = parser.make_parse_context(Parser::ParseContextMode::Syntax);
    auto* value = rust_parse_css_descriptor(&context.context, to_underlying(at_rule_id), ffi_utf16_view(descriptor.name()), ffi_utf16_view(parser.m_source));
    if (!value)
        return nullptr;
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(value));
}

}
