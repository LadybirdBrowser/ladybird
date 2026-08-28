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
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

void AtRule::for_each_as_declaration_list(DeclarationVisitor&& visit) const
{
    for (auto const& child : child_rules_and_lists_of_declarations) {
        if (!child.has<Vector<Declaration>>())
            continue;
        for (auto const& declaration : child.get<Vector<Declaration>>())
            visit(declaration);
    }
}

void AtRule::for_each_as_qualified_rule_list(QualifiedRuleVisitor&& visit) const
{
    for (auto const& child : child_rules_and_lists_of_declarations) {
        if (!child.has<Rule>() || !child.get<Rule>().has<QualifiedRule>())
            continue;
        auto const& qualified_rule = child.get<Rule>().get<QualifiedRule>();
        if (qualified_rule.kind == ValueParserFFI::FfiRuleKind::Qualified)
            visit(qualified_rule);
    }
}

void AtRule::for_each_as_declaration_rule_list(AtRuleVisitor&& visit_at_rule, DeclarationVisitor&& visit_declaration) const
{
    for (auto const& child : child_rules_and_lists_of_declarations) {
        child.visit(
            [&](Rule const& rule) {
                if (!rule.has<AtRule>())
                    return;
                auto const& at_rule = rule.get<AtRule>();
                if (at_rule.kind != ValueParserFFI::FfiRuleKind::Invalid)
                    visit_at_rule(at_rule);
            },
            [&](Vector<Declaration> const& declarations) {
                for (auto const& declaration : declarations)
                    visit_declaration(declaration);
            });
    }
}

void QualifiedRule::for_each_as_declaration_list(DeclarationVisitor&& visit) const
{
    for (auto const& declaration : declarations)
        visit(declaration);

    for (auto const& child : child_rules) {
        child.visit(
            [&](Rule const&) {},
            [&](Vector<Declaration> const& declarations) {
                for (auto const& declaration : declarations)
                    visit(declaration);
            });
    }
}

using namespace ValueParserFFI;

static Utf16View utf16_value(FfiSyntaxParseData const& data, size_t offset, size_t length)
{
    VERIFY(offset <= data.value_count);
    VERIFY(length <= data.value_count - offset);
    return { reinterpret_cast<char16_t const*>(data.values + offset), length };
}

PageSelectorList page_selector_list_from_rust(FfiPageSelectorListData const& data)
{
    PageSelectorList result;
    for (size_t index = 0; index < data.selector_count; ++index) {
        auto const& selector = data.selectors[index];
        Optional<Utf16FlyString> name;
        if (selector.name_offset != NumericLimits<size_t>::max()) {
            VERIFY(selector.name_offset <= data.value_count && selector.name_length <= data.value_count - selector.name_offset);
            name = Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(data.values + selector.name_offset), selector.name_length });
        }
        VERIFY(selector.pseudo_classes_start <= data.pseudo_class_count && selector.pseudo_class_count <= data.pseudo_class_count - selector.pseudo_classes_start);
        Vector<PagePseudoClass> pseudo_classes;
        for (size_t pseudo_class_index = 0; pseudo_class_index < selector.pseudo_class_count; ++pseudo_class_index) {
            auto pseudo_class = data.pseudo_classes[selector.pseudo_classes_start + pseudo_class_index];
            VERIFY(pseudo_class <= FfiPageSelectorItemKind::Blank);
            pseudo_classes.append(static_cast<PagePseudoClass>(pseudo_class));
        }
        result.empend(move(name), move(pseudo_classes));
    }
    return result;
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
        VERIFY(to_underlying(diagnostic.code) <= to_underlying(FfiSyntaxDiagnosticCode::InvalidRuleContext));
        auto required_value = [&](size_t offset, size_t length) {
            VERIFY(offset != NumericLimits<size_t>::max());
            return utf16_value(data, offset, length);
        };
        auto report_invalid_rule = [&](String description) {
            ErrorReporter::the().report(InvalidRuleError {
                .rule_name = Utf16FlyString::from_utf16(required_value(diagnostic.primary_offset, diagnostic.primary_length)),
                .prelude = MUST(required_value(diagnostic.prelude_offset, diagnostic.prelude_length).to_utf8(AllowLonelySurrogates::Yes)),
                .description = move(description),
            });
        };
        switch (diagnostic.code) {
        case FfiSyntaxDiagnosticCode::BadString:
        case FfiSyntaxDiagnosticCode::BadUrl: {
            auto start = source_position(diagnostic.start_line, diagnostic.start_column);
            auto end = source_position(diagnostic.end_line, diagnostic.end_column);
            ErrorReporter::the().report(SyntaxDiagnosticError {
                .code = diagnostic.code == FfiSyntaxDiagnosticCode::BadString ? SyntaxDiagnosticCode::BadString : SyntaxDiagnosticCode::BadUrl,
                .start_line = start.line,
                .start_column = start.column,
                .end_line = end.line,
                .end_column = end.column,
            });
            break;
        }
        case FfiSyntaxDiagnosticCode::UnknownRule:
            ErrorReporter::the().report(UnknownRuleError {
                .rule_name = Utf16FlyString::from_utf16(required_value(diagnostic.primary_offset, diagnostic.primary_length)),
            });
            break;
        case FfiSyntaxDiagnosticCode::StyleSelectorsInvalid:
            report_invalid_rule("Selectors invalid."_string);
            break;
        case FfiSyntaxDiagnosticCode::StyleEmptySelector:
            report_invalid_rule("Empty selector."_string);
            break;
        case FfiSyntaxDiagnosticCode::LayerInvalidName:
            report_invalid_rule("Not a valid layer name."_string);
            break;
        case FfiSyntaxDiagnosticCode::LayerContainsInvalidName:
            report_invalid_rule("Contains invalid layer name."_string);
            break;
        case FfiSyntaxDiagnosticCode::KeyframesMustBeBlock:
            report_invalid_rule("Must be a block, not a statement."_string);
            break;
        case FfiSyntaxDiagnosticCode::KeyframesInvalidName:
            report_invalid_rule("Invalid keyframes name."_string);
            break;
        case FfiSyntaxDiagnosticCode::NamespaceMustBeStatement:
            report_invalid_rule("Must be a statement, not a block."_string);
            break;
        case FfiSyntaxDiagnosticCode::NamespaceInvalidPrelude:
            report_invalid_rule("Invalid namespace prelude."_string);
            break;
        case FfiSyntaxDiagnosticCode::MediaExpectedBlock:
            report_invalid_rule("Expected a block."_string);
            break;
        case FfiSyntaxDiagnosticCode::SupportsMustBeBlock:
        case FfiSyntaxDiagnosticCode::ContainerMustBeBlock:
        case FfiSyntaxDiagnosticCode::CounterStyleMustBeBlock:
        case FfiSyntaxDiagnosticCode::FontFaceMustBeBlock:
        case FfiSyntaxDiagnosticCode::FontFeatureValuesMustBeBlock:
        case FfiSyntaxDiagnosticCode::FunctionMustBeBlock:
        case FfiSyntaxDiagnosticCode::PageMustBeBlock:
        case FfiSyntaxDiagnosticCode::MarginMustBeBlock:
            report_invalid_rule("Must be a block, not a statement."_string);
            break;
        case FfiSyntaxDiagnosticCode::SupportsClauseInvalid:
            report_invalid_rule("Supports clause invalid."_string);
            break;
        case FfiSyntaxDiagnosticCode::ContainerConditionsInvalid:
            report_invalid_rule("Invalid container condition list."_string);
            break;
        case FfiSyntaxDiagnosticCode::CounterStyleMissingName:
            report_invalid_rule("Missing counter style name."_string);
            break;
        case FfiSyntaxDiagnosticCode::FontFacePreludeNotAllowed:
        case FfiSyntaxDiagnosticCode::MarginPreludeNotAllowed:
            report_invalid_rule("Prelude is not allowed."_string);
            break;
        case FfiSyntaxDiagnosticCode::InvalidRuleLocation:
            ErrorReporter::the().report(InvalidRuleLocationError {
                .outer_rule_name = Utf16FlyString::from_utf16(required_value(diagnostic.primary_offset, diagnostic.primary_length)),
                .inner_rule_name = Utf16FlyString::from_utf16(required_value(diagnostic.secondary_offset, diagnostic.secondary_length)),
            });
            break;
        case FfiSyntaxDiagnosticCode::ImportInvalid:
        case FfiSyntaxDiagnosticCode::KeyframeSelectorsInvalid:
        case FfiSyntaxDiagnosticCode::FontFeatureValuesPreludeInvalid:
        case FfiSyntaxDiagnosticCode::FunctionPreludeInvalid:
        case FfiSyntaxDiagnosticCode::PagePreludeInvalid:
        case FfiSyntaxDiagnosticCode::PropertyPreludeInvalid:
        case FfiSyntaxDiagnosticCode::ScopeInvalid:
        case FfiSyntaxDiagnosticCode::MisplacedImport:
        case FfiSyntaxDiagnosticCode::MisplacedNamespace:
        case FfiSyntaxDiagnosticCode::InvalidRuleContext:
            break;
        }
    }
}

static Declaration declaration(FfiSyntaxParseData const& data, size_t index)
{
    VERIFY(index < data.declaration_count);
    auto const& declaration = data.declarations[index];
    VERIFY(declaration.rejection <= FfiDeclarationRejection::InvalidValue);
    auto name_view = utf16_value(data, declaration.name_offset, declaration.name_length);
    auto value_view = utf16_value(data, declaration.value_source_offset, declaration.value_source_length);
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
            ? DescriptorNameAndID::from_custom_name(Utf16FlyString::from_utf16(name_view))
            : DescriptorNameAndID::from_id(descriptor_id);
    }

    Optional<StylePropertyAndName> property;
    if (declaration.parsed_value) {
        VERIFY(declaration.rejection == FfiDeclarationRejection::None);
        auto value = StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(declaration.parsed_value));
        if (declaration.is_property) {
            VERIFY(parsed_property_id.has_value());
            auto custom_name = *parsed_property_id == PropertyID::Custom ? Utf16FlyString::from_utf16(name_view) : Utf16FlyString {};
            property = StylePropertyAndName {
                .property = { declaration.important ? Important::Yes : Important::No, *parsed_property_id, move(value) },
                .name = move(custom_name),
            };
        } else
            parsed_value = move(value);
    } else {
        VERIFY(!parsed_property_id.has_value() || declaration.rejection == FfiDeclarationRejection::InvalidValue);
    }

    switch (declaration.rejection) {
    case FfiDeclarationRejection::None:
    case FfiDeclarationRejection::IgnoredVendorPrefix:
        break;
    case FfiDeclarationRejection::UnknownProperty:
        ErrorReporter::the().report(UnknownPropertyError { .property_name = Utf16FlyString::from_utf16(name_view) });
        break;
    case FfiDeclarationRejection::InvalidValue:
        VERIFY(parsed_property_id.has_value());
        ErrorReporter::the().report(InvalidPropertyError {
            .property_name = string_from_property_id(*parsed_property_id),
            .value_string = MUST(value_view.to_utf8(AllowLonelySurrogates::Yes)),
            .description = "Failed to parse."_string,
        });
        break;
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
    Optional<Utf16FlyString> name;
    if (declaration.preserve_source_text || font_feature_values.has_value())
        name = Utf16FlyString::from_utf16(name_view);
    return Declaration {
        .name = move(name),
        .important = declaration.important ? Important::Yes : Important::No,
        .source_position = source_position(declaration.start_line, declaration.start_column),
        .value_text = declaration.preserve_source_text ? Optional<Utf16String> { Utf16String::from_utf16(value_view) } : OptionalNone {},
        .parsed_property_id = parsed_property_id,
        .property = move(property),
        .rejection = declaration.rejection,
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
    auto kind = static_cast<ParsedRulePreludeKind>(rule.parsed_prelude_kind);
    VERIFY(rule.parsed_prelude_items_start <= data.prelude_item_count);
    VERIFY(rule.parsed_prelude_item_count <= data.prelude_item_count - rule.parsed_prelude_items_start);
    VERIFY((kind == ParsedRulePreludeKind::PageSelectors) == (rule.page_selector_list != nullptr));
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
        .kind = kind,
        .name = optional_string(rule.parsed_prelude_name_offset, rule.parsed_prelude_name_length),
        .secondary = optional_string(rule.parsed_prelude_secondary_offset, rule.parsed_prelude_secondary_length),
        .syntax = rule.parsed_prelude_syntax ? Optional<RustSyntaxHandle> { RustSyntaxHandle { rule.parsed_prelude_syntax } } : OptionalNone {},
        .items = move(items),
        .page_selectors = rule.page_selector_list ? page_selector_list_from_rust(rust_page_selector_list_data(rule.page_selector_list)) : PageSelectorList {},
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
    auto children = items(data, rule.children_start, rule.child_count);
    if (rule.rule_type == 0) {
        return AtRule {
            .kind = rule.rule_kind,
            .name = Utf16FlyString::from_utf16(utf16_value(data, rule.name_offset, rule.name_length)),
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
        .kind = rule.rule_kind,
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
    auto* parse = rust_parse_css_stylesheet_syntax(ffi_utf16_view(parser.m_source), &context.context);
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
    auto* parse = rust_parse_css_rule_syntax(ffi_utf16_view(parser.m_source), reinterpret_cast<u8 const*>(contexts.data()), contexts.size(), nested == RuleNesting::Yes, &context.context);
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
    auto* parse = rust_parse_css_keyframe_selectors_syntax(ffi_utf16_view(parser.m_source), &context.context);
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
    auto* parse = rust_parse_css_block_syntax(ffi_utf16_view(source), reinterpret_cast<u8 const*>(contexts.data()), contexts.size(), &context.context, preserve_property_source_text == PreservePropertySourceText::Yes);
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
