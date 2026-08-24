/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/Utf16View.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/SVG/AttributeParser.h>
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

static FfiUtf16View ffi_utf16_view(Utf16View view)
{
    return {
        .ascii = view.has_ascii_storage() ? reinterpret_cast<u8 const*>(view.ascii_span().data()) : nullptr,
        .utf16 = view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(view.utf16_span().data()),
        .length = view.length_in_code_units(),
    };
}

static size_t retain_utf16_fly_string(u16 const* code_units, size_t length)
{
    return Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(code_units), length }).to_raw_leaked();
}

static size_t normalize_svg_path_data(u16 const* code_units, size_t length)
{
    auto path = SVG::AttributeParser::parse_path_data(Utf16View { reinterpret_cast<char16_t const*>(code_units), length });
    if (path.instructions().is_empty())
        return 0;
    return Utf16String::from_utf8(path.serialize()).to_raw_leaked();
}

static bool rust_font_format_is_supported(u16 const* code_units, size_t length)
{
    return font_format_is_supported(Utf16View { reinterpret_cast<char16_t const*>(code_units), length });
}

static bool rust_font_tech_is_supported(u8 tech)
{
    return font_tech_is_supported(static_cast<FontTech>(tech));
}

static u16 resolve_property_id(u16 const* code_units, size_t length)
{
    auto name = Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(code_units), length });
    auto property = PropertyNameAndID::from_name(move(name));
    return property.has_value() ? to_underlying(property->id()) : NumericLimits<u16>::max();
}

static bool resolve_descriptor_integer(void const* document_pointer, void const* value_pointer, i32* result)
{
    auto const* document = static_cast<DOM::Document const*>(document_pointer);
    if (!document || !result)
        return false;
    auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
        static_cast<StyleValueFFI::StyleValueData const*>(value_pointer)));
    if (value->is_integer()) {
        *result = value->as_integer().integer();
        return true;
    }
    if (!value->is_calculated())
        return false;
    auto absolutized = value->absolutized(ComputationContext { .length_resolution_context = Length::ResolutionContext::for_document(*document) });
    if (absolutized->is_integer()) {
        *result = absolutized->as_integer().integer();
        return true;
    }
    if (!absolutized->is_calculated())
        return false;
    auto resolved = absolutized->as_calculated().resolve_integer({});
    if (!resolved.has_value())
        return false;
    *result = resolved.value();
    return true;
}

struct ParseContextStorage {
    ValueParserFFI::ParseContext context {};
};

static ParseContextStorage make_parse_context(bool in_quirks_mode, bool is_svg_presentation_attribute, ReadonlyBytes document_url, ReadonlyBytes document_base_url, DOM::Document const* document, size_t& random_function_index)
{
    return {
        .context = {
            .in_quirks_mode = in_quirks_mode,
            .is_svg_presentation_attribute = is_svg_presentation_attribute,
            .is_substituted_value = false,
            .contains_attr_tainted_values = false,
            .value_contexts = nullptr,
            .value_context_count = 0,
            .document_url = document_url.data(),
            .document_url_length = document_url.size(),
            .document_base_url = document_base_url.data(),
            .document_base_url_length = document_base_url.size(),
            .intern_utf16_fly_string = retain_utf16_fly_string,
            .normalize_svg_path_data = normalize_svg_path_data,
            .precomputed_svg_paths = nullptr,
            .precomputed_svg_path_count = 0,
            .font_format_is_supported = rust_font_format_is_supported,
            .font_tech_is_supported = rust_font_tech_is_supported,
            .descriptor_integer_resolution_context = document,
            .resolve_descriptor_integer = resolve_descriptor_integer,
            .random_function_index = &random_function_index,
        },
    };
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
    RefPtr<StyleValue const> parsed_value;
    if (declaration.is_property) {
        if (declaration.property_id != NumericLimits<u16>::max())
            parsed_property_id = static_cast<PropertyID>(declaration.property_id);
    }
    if (declaration.parse_status == FfiParseStatus::Parsed) {
        VERIFY(declaration.parsed_value);
        parsed_value = StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(declaration.parsed_value));
    } else {
        VERIFY(!declaration.parsed_value);
    }
    return Declaration {
        .name = Utf16FlyString::from_utf16(utf16_value(data, declaration.name_offset, declaration.name_length)),
        .important = declaration.important ? Important::Yes : Important::No,
        .original_value_text = move(original_value_text),
        .original_full_text = optional_text(declaration.original_full_text_offset, declaration.original_full_text_length),
        .source_position = source_position(declaration.start_line, declaration.start_column),
        .value_text = move(value_text),
        .parsed_property_id = parsed_property_id,
        .parsed_value = move(parsed_value),
    };
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
    VERIFY(rule.parsed_prelude_kind <= to_underlying(ParsedRulePreludeKind::Function));
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
        items.unchecked_append({
            .value = optional_string(item.value_offset, item.value_length),
            .number_value = item.number_value,
            .flags = item.flags,
        });
    }
    return {
        .kind = static_cast<ParsedRulePreludeKind>(rule.parsed_prelude_kind),
        .name = optional_string(rule.parsed_prelude_name_offset, rule.parsed_prelude_name_length),
        .secondary = optional_string(rule.parsed_prelude_secondary_offset, rule.parsed_prelude_secondary_length),
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
            .name = Utf16FlyString::from_utf16(utf16_value(data, rule.name_offset, rule.name_length)),
            .prelude_text = move(prelude_text),
            .parsed_prelude = parsed_rule_prelude(data, rule),
            .child_rules_and_lists_of_declarations = move(children),
            .is_block_rule = rule.has_block,
        };
    }

    VERIFY(rule.rule_type == 1);
    return QualifiedRule {
        .prelude_text = move(prelude_text),
        .parsed_prelude = parsed_rule_prelude(data, rule),
        .declarations = declarations(data, rule.declarations_start, rule.declaration_count),
        .child_rules = move(children),
        .source_position = rule.has_source_position ? Optional<SourcePosition> { source_position(rule.start_line, rule.start_column) } : OptionalNone {},
    };
}

Vector<Rule> RustSyntaxParser::parse_stylesheet(Parser& parser)
{
    ReadonlyBytes document_url;
    ReadonlyBytes document_base_url;
    if (parser.m_document) {
        if (!parser.m_serialized_document_url.has_value())
            parser.m_serialized_document_url = parser.m_document->url().serialize();
        if (!parser.m_serialized_document_base_url.has_value())
            parser.m_serialized_document_base_url = parser.m_document->base_url().serialize();
        document_url = parser.m_serialized_document_url->bytes();
        document_base_url = parser.m_serialized_document_base_url->bytes();
    }
    auto context = make_parse_context(parser.in_quirks_mode(), parser.is_parsing_svg_presentation_attribute(), document_url, document_base_url, parser.m_document.ptr(), parser.m_random_function_index);
    auto* parse = rust_parse_css_stylesheet_syntax(ffi_utf16_view(parser.m_source), &context.context, resolve_property_id);
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
    ReadonlyBytes document_url;
    ReadonlyBytes document_base_url;
    if (parser.m_document) {
        if (!parser.m_serialized_document_url.has_value())
            parser.m_serialized_document_url = parser.m_document->url().serialize();
        if (!parser.m_serialized_document_base_url.has_value())
            parser.m_serialized_document_base_url = parser.m_document->base_url().serialize();
        document_url = parser.m_serialized_document_url->bytes();
        document_base_url = parser.m_serialized_document_base_url->bytes();
    }
    auto context = make_parse_context(parser.in_quirks_mode(), parser.is_parsing_svg_presentation_attribute(), document_url, document_base_url, parser.m_document.ptr(), parser.m_random_function_index);
    static_assert(sizeof(RuleContext) == sizeof(u8));
    auto* parse = rust_parse_css_rule_syntax(ffi_utf16_view(parser.m_source), reinterpret_cast<u8 const*>(contexts.data()), contexts.size(), nested == RuleNesting::Yes, &context.context, resolve_property_id);
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
    ReadonlyBytes document_url;
    ReadonlyBytes document_base_url;
    if (parser.m_document) {
        if (!parser.m_serialized_document_url.has_value())
            parser.m_serialized_document_url = parser.m_document->url().serialize();
        if (!parser.m_serialized_document_base_url.has_value())
            parser.m_serialized_document_base_url = parser.m_document->base_url().serialize();
        document_url = parser.m_serialized_document_url->bytes();
        document_base_url = parser.m_serialized_document_base_url->bytes();
    }
    auto context = make_parse_context(parser.in_quirks_mode(), parser.is_parsing_svg_presentation_attribute(), document_url, document_base_url, parser.m_document.ptr(), parser.m_random_function_index);
    auto* parse = rust_parse_css_keyframe_selectors_syntax(ffi_utf16_view(parser.m_source), &context.context, resolve_property_id);
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
    ReadonlyBytes document_url;
    ReadonlyBytes document_base_url;
    if (parser.m_document) {
        if (!parser.m_serialized_document_url.has_value())
            parser.m_serialized_document_url = parser.m_document->url().serialize();
        if (!parser.m_serialized_document_base_url.has_value())
            parser.m_serialized_document_base_url = parser.m_document->base_url().serialize();
        document_url = parser.m_serialized_document_url->bytes();
        document_base_url = parser.m_serialized_document_base_url->bytes();
    }
    auto context = make_parse_context(parser.in_quirks_mode(), parser.is_parsing_svg_presentation_attribute(), document_url, document_base_url, parser.m_document.ptr(), parser.m_random_function_index);
    auto* parse = rust_parse_css_block_syntax(ffi_utf16_view(source), reinterpret_cast<u8 const*>(contexts.data()), contexts.size(), &context.context, resolve_property_id, preserve_property_source_text == PreservePropertySourceText::Yes);
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
    ReadonlyBytes document_url;
    ReadonlyBytes document_base_url;
    if (parser.m_document) {
        if (!parser.m_serialized_document_url.has_value())
            parser.m_serialized_document_url = parser.m_document->url().serialize();
        if (!parser.m_serialized_document_base_url.has_value())
            parser.m_serialized_document_base_url = parser.m_document->base_url().serialize();
        document_url = parser.m_serialized_document_url->bytes();
        document_base_url = parser.m_serialized_document_base_url->bytes();
    }
    auto context = make_parse_context(parser.in_quirks_mode(), parser.is_parsing_svg_presentation_attribute(), document_url, document_base_url, parser.m_document.ptr(), parser.m_random_function_index);
    auto* value = rust_parse_css_descriptor(&context.context, to_underlying(at_rule_id), ffi_utf16_view(descriptor.name()), ffi_utf16_view(parser.m_source));
    if (!value)
        return nullptr;
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(value));
}

}
