/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibCore/EventLoop.h>
#include <LibGC/Root.h>
#include <LibThreading/ThreadPool.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/Parser/SourcePosition.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

using namespace ValueParserFFI;

static Utf16View utf16_value(u16 const* values, size_t value_count, size_t offset, size_t length)
{
    VERIFY(offset <= value_count);
    VERIFY(length <= value_count - offset);
    return { reinterpret_cast<char16_t const*>(values + offset), length };
}

static SourcePosition source_position(size_t line, size_t column)
{
    VERIFY(line <= NumericLimits<u32>::max());
    VERIFY(column <= NumericLimits<u32>::max());
    return { static_cast<u32>(line), static_cast<u32>(column) };
}

static void report_diagnostics(RustStyleSheetParse const& parse)
{
    rust_css_syntax_visit_diagnostics(parse.handle(), [](u16 const* values, size_t value_count, FfiSyntaxDiagnostic const* diagnostic_pointer) {
        auto const& diagnostic = *diagnostic_pointer;
        VERIFY(to_underlying(diagnostic.code) <= to_underlying(FfiSyntaxDiagnosticCode::InvalidRuleContext));
        auto required_value = [&](size_t offset, size_t length) {
            VERIFY(offset != NumericLimits<size_t>::max());
            return utf16_value(values, value_count, offset, length);
        };
        auto report_invalid_rule = [&](String description) {
            ErrorReporter::the().report(InvalidRuleError {
                .rule_name = Utf16FlyString::from_utf16(required_value(diagnostic.primary_offset, diagnostic.primary_length)),
                .prelude = Utf16String::from_utf16(required_value(diagnostic.prelude_offset, diagnostic.prelude_length)),
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
    });
}

static void report_declaration_error(u16 const* values, size_t value_count, FfiSyntaxDeclaration const* declaration_pointer)
{
    auto const& declaration = *declaration_pointer;
    auto name_view = utf16_value(values, value_count, declaration.name_offset, declaration.name_length);
    auto value_view = utf16_value(values, value_count, declaration.value_source_offset, declaration.value_source_length);
    switch (declaration.rejection) {
    case FfiDeclarationRejection::None:
    case FfiDeclarationRejection::IgnoredVendorPrefix:
        break;
    case FfiDeclarationRejection::UnknownProperty:
        ErrorReporter::the().report(UnknownPropertyError { .property_name = Utf16FlyString::from_utf16(name_view) });
        break;
    case FfiDeclarationRejection::InvalidValue:
        VERIFY(declaration.property_id != NumericLimits<u16>::max());
        ErrorReporter::the().report(InvalidPropertyError {
            .property_name = string_from_property_id(static_cast<PropertyID>(declaration.property_id)),
            .value_string = Utf16String::from_utf16(value_view),
            .description = "Failed to parse."_string,
        });
        break;
    }
}

RustStyleSheetParse Parser::parse_stylesheet(Utf16View source)
{
    auto context = make_parse_context(ParseContextMode::Syntax);
    return RustStyleSheetParse { rust_parse_css_stylesheet_syntax(ffi_utf16_view(source), &context.context) };
}

void Parser::parse_stylesheet_off_thread(ParsingParams const& params, Utf16String source, Function<void(RustStyleSheetParse)> on_complete)
{
    auto parser = adopt_own(*new Parser(params));
    auto context = make<Parser::ParseContextStorage>(*parser, Parser::ParseContextMode::Syntax, Optional<PropertyID> {});
    auto input = ffi_utf16_view(source);
    auto const* parse_context = &context->context;

    // NB: Retain all borrowed input storage and GC roots on the main thread. The worker only
    //     accesses the immutable source and context snapshots, plus this private parser's counter.
    auto* callback = new Function<void(RustStyleSheetParse)>(
        [source = move(source), parser = move(parser), context = move(context), document = GC::Root<DOM::Document const>::create(params.document.ptr()), on_complete = move(on_complete)](RustStyleSheetParse result) mutable {
            on_complete(move(result));
        });
    auto& main_thread_event_loop = Core::EventLoop::current();
    Threading::ThreadPool::the().submit([input, parse_context, callback, &main_thread_event_loop] {
        RustStyleSheetParse result { rust_parse_css_stylesheet_syntax(input, parse_context) };
        main_thread_event_loop.deferred_invoke([result = move(result), callback]() mutable {
            (*callback)(move(result));
            delete callback;
        });
    });
}

RustRuleList RustStyleSheetParse::native_rules() const
{
    report_diagnostics(*this);
    rust_css_syntax_visit_declaration_errors(handle(), report_declaration_error);
    return RustRuleList { rust_css_syntax_native_rules(handle()) };
}

Optional<RustRule> Parser::parse_as_css_rule(Utf16View source, bool nested)
{
    auto context = make_parse_context(ParseContextMode::Syntax);
    static_assert(sizeof(RuleContext) == sizeof(u8));
    RustStyleSheetParse parse { rust_parse_css_rule_syntax(ffi_utf16_view(source), reinterpret_cast<u8 const*>(m_rule_context.data()), m_rule_context.size(), nested, &context.context) };
    report_diagnostics(parse);
    if (rust_css_syntax_root_count(parse.handle()) != 1)
        return {};
    rust_css_syntax_visit_declaration_errors(parse.handle(), report_declaration_error);
    auto rules = RustRuleList { rust_css_syntax_native_rules(parse.handle()) };
    if (rules.size() != 1)
        return {};
    return rules.at(0);
}

Optional<RustRule> Parser::parse_as_keyframe_rule(Utf16View source)
{
    m_rule_context.append(RuleContext::AtKeyframes);
    ScopeGuard guard = [&] { m_rule_context.take_last(); };
    auto context = make_parse_context(ParseContextMode::Syntax);
    static_assert(sizeof(RuleContext) == sizeof(u8));
    RustStyleSheetParse parse { rust_parse_css_block_syntax(ffi_utf16_view(source), reinterpret_cast<u8 const*>(m_rule_context.data()), m_rule_context.size(), &context.context, false) };
    report_diagnostics(parse);
    if (rust_css_syntax_root_count(parse.handle()) != 1)
        return {};
    auto rules = RustRuleList { rust_css_syntax_native_rules(parse.handle()) };
    if (rules.size() != 1 || rules.at(0).type() != RustRule::Type::Keyframe)
        return {};
    return rules.at(0);
}

RustDeclarationBlock Parser::parse_as_property_declaration_block(Utf16View source)
{
    static_assert(sizeof(RuleContext) == sizeof(u8));
    auto context = make_parse_context(ParseContextMode::Syntax);
    // https://drafts.csswg.org/cssom/#parse-a-css-declaration-block
    // 1. Let declarations be the returned declarations from invoking parse a block’s contents with string.
    auto* handle = rust_parse_css_block_syntax(ffi_utf16_view(source), reinterpret_cast<u8 const*>(m_rule_context.data()), m_rule_context.size(), &context.context, false);
    RustStyleSheetParse parse { handle };
    report_diagnostics(parse);
    rust_css_syntax_visit_declaration_errors(parse.handle(), report_declaration_error);
    return RustDeclarationBlock { rust_css_syntax_parse_declaration_block(handle) };
}

RustDescriptorBlock Parser::parse_as_descriptor_declaration_block(Utf16View source, AtRuleID at_rule_id)
{
    auto context_type = [at_rule_id] {
        switch (at_rule_id) {
        case AtRuleID::FontFace:
            return RuleContext::AtFontFace;
        case AtRuleID::Function:
            return RuleContext::AtFunction;
        case AtRuleID::Page:
            return RuleContext::AtPage;
        case AtRuleID::Property:
            return RuleContext::AtProperty;
        case AtRuleID::CounterStyle:
            // NB: We don't actually have a `CSSDescriptors` for `@counter-style` so this function shouldn't ever be
            //     called with `AtRuleID::CounterStyle`.
            VERIFY_NOT_REACHED();
        }
        VERIFY_NOT_REACHED();
    }();

    m_rule_context.append(context_type);
    ScopeGuard guard = [&] { m_rule_context.take_last(); };

    static_assert(sizeof(RuleContext) == sizeof(u8));
    auto context = make_parse_context(ParseContextMode::Syntax);
    auto* handle = rust_parse_css_block_syntax(ffi_utf16_view(source), reinterpret_cast<u8 const*>(m_rule_context.data()), m_rule_context.size(), &context.context, false);
    RustStyleSheetParse parse { handle };
    report_diagnostics(parse);
    rust_css_syntax_visit_declaration_errors(parse.handle(), report_declaration_error);
    return RustDescriptorBlock { rust_css_syntax_parse_descriptor_block(handle) };
}

Vector<DevToolsStyleDeclaration> Parser::parse_as_devtools_property_declaration_block(Utf16View source)
{
    static_assert(sizeof(RuleContext) == sizeof(u8));
    auto context = make_parse_context(ParseContextMode::Syntax);
    RustStyleSheetParse parse { rust_parse_css_block_syntax(ffi_utf16_view(source), reinterpret_cast<u8 const*>(m_rule_context.data()), m_rule_context.size(), &context.context, true) };
    report_diagnostics(parse);
    Vector<DevToolsStyleDeclaration> result;
    rust_css_syntax_visit_root_declarations(parse.handle(), &result, [](void* context, u16 const* values, size_t value_count, FfiSyntaxDeclaration const* declaration_pointer) {
        auto& result = *static_cast<Vector<DevToolsStyleDeclaration>*>(context);
        auto const& declaration = *declaration_pointer;
        report_declaration_error(values, value_count, &declaration);
        VERIFY(declaration.preserve_source_text);
        result.append(DevToolsStyleDeclaration {
            .name = Utf16FlyString::from_utf16(utf16_value(values, value_count, declaration.name_offset, declaration.name_length)),
            .value = Utf16String::from_utf16(utf16_value(values, value_count, declaration.value_source_offset, declaration.value_source_length)),
            .important = declaration.important ? Important::Yes : Important::No,
            .is_custom_property = declaration.is_property && declaration.property_id == to_underlying(PropertyID::Custom),
            .is_name_valid = declaration.rejection == FfiDeclarationRejection::None || declaration.rejection == FfiDeclarationRejection::InvalidValue,
            .is_valid = declaration.is_property && declaration.parsed_value,
        });
    });
    return result;
}

RefPtr<StyleValue const> Parser::parse_as_descriptor_value(Utf16View source, AtRuleID at_rule_id, DescriptorNameAndID const& descriptor)
{
    auto context = make_parse_context(ParseContextMode::Syntax);
    auto* value = rust_parse_css_descriptor(&context.context, to_underlying(at_rule_id), ffi_utf16_view(descriptor.name()), ffi_utf16_view(source));
    if (!value)
        return nullptr;
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(value));
}

}
