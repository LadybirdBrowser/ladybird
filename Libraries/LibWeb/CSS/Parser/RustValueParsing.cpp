/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/StringBuilder.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustSyntaxHandle.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/StyleComputeFFI.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

Parser::ParseContextStorage::ParseContextStorage(Parser& parser, ParseContextMode mode, Optional<PropertyID> direct_property_context)
{
    if (mode != ParseContextMode::Syntax) {
        for (auto const& value_context : parser.m_value_context) {
            ValueParserFFI::FfiValueParsingContext ffi_context {};
            value_context.visit(
                [&](PropertyID property_id) {
                    ffi_context.kind = ValueParserFFI::FfiValueParsingContextKind::Property;
                    ffi_context.value = to_underlying(property_id);
                },
                [&](SpecialContext special_context) {
                    ffi_context.kind = ValueParserFFI::FfiValueParsingContextKind::Special;
                    ffi_context.value = to_underlying(special_context);
                });
            value_contexts.append(ffi_context);
        }
        if (direct_property_context.has_value())
            value_contexts.append({ .kind = ValueParserFFI::FfiValueParsingContextKind::Property, .value = to_underlying(*direct_property_context), .secondary_value = 0, .name = {} });
        context.value_contexts = value_contexts.data();
        context.value_context_count = value_contexts.size();
    }

    ReadonlyBytes document_url;
    ReadonlyBytes document_base_url;
    if (parser.m_document) {
        if (!parser.m_serialized_document_url.has_value())
            parser.m_serialized_document_url = parser.m_document->url().serialize();
        if (!parser.m_serialized_document_base_url.has_value())
            parser.m_serialized_document_base_url = parser.m_document->base_url().serialize();
        document_url = parser.m_serialized_document_url->bytes();
        document_base_url = parser.m_serialized_document_base_url->bytes();
        if (mode == ParseContextMode::Syntax) {
            length_resolution_context = to_ffi_length_resolution_context_with_container_bases(
                Length::ResolutionContext::for_document(*parser.m_document), all_container_relative_length_units_mask);
            length_resolution_context->resolved_viewport_relative_length = nullptr;
        }
    }

    context = {
        .in_quirks_mode = parser.in_quirks_mode(),
        .is_svg_presentation_attribute = parser.is_parsing_svg_presentation_attribute(),
        .is_substituted_value = false,
        .contains_attr_tainted_values = false,
        .is_ua_style_sheet = mode == ParseContextMode::Syntax && parser.m_is_ua_style_sheet == IsUAStyleSheet::Yes,
        .value_contexts = context.value_contexts,
        .value_context_count = context.value_context_count,
        .declared_namespaces = mode == ParseContextMode::Syntax ? parser.m_declared_namespaces.handle() : nullptr,
        .document_url = document_url.data(),
        .document_url_length = document_url.size(),
        .document_base_url = document_base_url.data(),
        .document_base_url_length = document_base_url.size(),
        .length_resolution_context = length_resolution_context.has_value() ? &*length_resolution_context : nullptr,
        .random_function_index = &parser.m_random_function_index,
    };
}

Parser::ParseContextStorage Parser::make_parse_context(ParseContextMode mode, Optional<PropertyID> direct_property_context)
{
    return ParseContextStorage { *this, mode, direct_property_context };
}

Parser::ParseErrorOr<void> Parser::collect_arbitrary_substitution_function_presence(Utf16View source, SubstitutionFunctionsPresence& presence)
{
    u8 rust_presence = 0;
    if (!ValueParserFFI::rust_collect_arbitrary_substitution_function_presence_from_source(ffi_utf16_view(source), &rust_presence))
        return Parser::ParseError::SyntaxError;
    presence.attr |= rust_presence & (1 << 0);
    presence.dashed_function |= rust_presence & (1 << 1);
    presence.env |= rust_presence & (1 << 2);
    presence.if_ |= rust_presence & (1 << 3);
    presence.inherit |= rust_presence & (1 << 4);
    presence.var |= rust_presence & (1 << 5);
    return {};
}

Optional<RustSyntaxHandle> parse_as_syntax(Utf16View source, LimitSingleComponentIdentToCustomIdent limit_single_component_ident_to_custom_ident)
{
    auto syntax = ValueParserFFI::rust_parse_syntax(ffi_utf16_view(source), limit_single_component_ident_to_custom_ident == LimitSingleComponentIdentToCustomIdent::Yes);
    if (!syntax)
        return {};
    return RustSyntaxHandle { syntax };
}

NonnullRefPtr<StyleValue const> parse_with_a_syntax(ParsingParams const& parsing_params, Utf16View input, RustSyntaxHandle const& syntax)
{
    return Parser { parsing_params }.parse_with_a_syntax(input, syntax);
}

// https://drafts.csswg.org/css-values-5/#parse-with-a-syntax
NonnullRefPtr<StyleValue const> Parser::parse_with_a_syntax(Utf16View source, RustSyntaxHandle const& syntax)
{
    auto context = make_parse_context(ParseContextMode::RegisteredSyntax);
    ValueParserFFI::FfiParseStatus status { ValueParserFFI::FfiParseStatus::Invalid };
    auto parsed = ValueParserFFI::rust_parse_with_syntax(
        &context.context,
        ffi_utf16_view(source), syntax.data(), &status);
    if (status != ValueParserFFI::FfiParseStatus::Parsed)
        return StyleValue::create_guaranteed_invalid();
    VERIFY(parsed);
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(parsed));
}

Parser::ParseErrorOr<NonnullRefPtr<StyleValue const>> Parser::parse_css_value_from_source(PropertyID property_id, Utf16View source)
{
    ScopeGuard reset_random_index = [&] {
        if (!m_value_context.is_empty() && !m_value_context.find_first_index_if([](auto const& context) { return context.template has<PropertyID>(); }).has_value())
            m_random_function_index = 0;
    };
    auto context = make_parse_context(ParseContextMode::Value, property_id);
    ValueParserFFI::FfiParseStatus status { ValueParserFFI::FfiParseStatus::NotHandled };
    auto const* parsed_value = ValueParserFFI::rust_parse_css_value(
        &context.context, to_underlying(property_id), ffi_utf16_view(source), &status);

    if (status != ValueParserFFI::FfiParseStatus::Parsed) {
        if (status == ValueParserFFI::FfiParseStatus::NotHandled)
            warnln("Rust CSS value parser did not handle property {}", string_from_property_id(property_id));
        return ParseError::SyntaxError;
    }

    VERIFY(parsed_value);
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(parsed_value));
}

RefPtr<StyleValue const> Parser::parse_primitive_value_from_source(ValueType value_type, Utf16View source, NumericRange const& accepted_range)
{
    auto context = make_parse_context(ParseContextMode::Value);
    auto const* parsed = ValueParserFFI::rust_parse_entire_css_primitive_from_source(
        &context.context, to_underlying(value_type), ffi_utf16_view(source), accepted_range.min, accepted_range.max);
    if (!parsed)
        return nullptr;
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(parsed));
}

}
