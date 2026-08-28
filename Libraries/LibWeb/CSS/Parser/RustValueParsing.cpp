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
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

static size_t retain_utf16_fly_string(u16 const* code_units, size_t length)
{
    return Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(code_units), length }).to_raw_leaked();
}

Parser::ParseContextStorage::ParseContextStorage(Parser& parser, ParseContextMode mode, Optional<PropertyID> direct_property_context)
{
    if (mode != ParseContextMode::Syntax) {
        if (direct_property_context.has_value()) {
            VERIFY(parser.m_value_context.is_empty());
            single_property_context.kind = ValueParserFFI::FfiValueParsingContextKind::Property;
            single_property_context.value = to_underlying(*direct_property_context);
            context.value_contexts = &single_property_context;
            context.value_context_count = 1;
        } else if (parser.m_value_context.size() == 1 && parser.m_value_context[0].has<PropertyID>()) {
            single_property_context.kind = ValueParserFFI::FfiValueParsingContextKind::Property;
            single_property_context.value = to_underlying(parser.m_value_context[0].get<PropertyID>());
            context.value_contexts = &single_property_context;
            context.value_context_count = 1;
        } else {
            value_contexts.ensure_capacity(parser.m_value_context.size());
            for (auto const& value_context : parser.m_value_context) {
                ValueParserFFI::FfiValueParsingContext ffi_context {};
                value_context.visit(
                    [&](PropertyID property_id) {
                        ffi_context.kind = ValueParserFFI::FfiValueParsingContextKind::Property;
                        ffi_context.value = to_underlying(property_id);
                    },
                    [&](FunctionContext const& function_context) {
                        ffi_context.kind = ValueParserFFI::FfiValueParsingContextKind::Function;
                        ffi_context.name = ffi_utf16_view(function_context.name);
                    },
                    [&](DescriptorContext const& descriptor_context) {
                        ffi_context.kind = ValueParserFFI::FfiValueParsingContextKind::Descriptor;
                        ffi_context.value = to_underlying(descriptor_context.at_rule);
                        ffi_context.secondary_value = to_underlying(descriptor_context.descriptor);
                    },
                    [&](SpecialContext special_context) {
                        ffi_context.kind = ValueParserFFI::FfiValueParsingContextKind::Special;
                        ffi_context.value = to_underlying(special_context);
                    });
                value_contexts.append(ffi_context);
            }
            context.value_contexts = value_contexts.data();
            context.value_context_count = value_contexts.size();
        }
    }

    if (mode == ParseContextMode::Syntax) {
        declared_namespaces.ensure_capacity(parser.m_declared_namespaces.size());
        for (auto const& namespace_ : parser.m_declared_namespaces)
            declared_namespaces.unchecked_append(ffi_utf16_view(namespace_));
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
        .declared_namespaces = declared_namespaces.data(),
        .declared_namespace_count = declared_namespaces.size(),
        .document_url = document_url.data(),
        .document_url_length = document_url.size(),
        .document_base_url = document_base_url.data(),
        .document_base_url_length = document_base_url.size(),
        .intern_utf16_fly_string = retain_utf16_fly_string,
        .length_resolution_context = length_resolution_context.has_value() ? &*length_resolution_context : nullptr,
        .random_function_index = &parser.m_random_function_index,
    };
}

Parser::ParseContextStorage Parser::make_parse_context(ParseContextMode mode, Optional<PropertyID> direct_property_context)
{
    return ParseContextStorage { *this, mode, direct_property_context };
}

static Parser::ParseErrorOr<void> collect_substitution_function_presence_in_rust(Utf16View source, SubstitutionFunctionsPresence& presence)
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

Parser::ParseErrorOr<void> Parser::collect_arbitrary_substitution_function_presence(Utf16View source, SubstitutionFunctionsPresence& presence)
{
    return collect_substitution_function_presence_in_rust(source, presence);
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
    return Parser::create(parsing_params, input).parse_with_a_syntax(syntax);
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
        return GuaranteedInvalidStyleValue::create();
    VERIFY(parsed);
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(parsed));
}

Parser::ParseErrorOr<NonnullRefPtr<StyleValue const>> Parser::parse_css_value_from_source(PropertyID property_id, Utf16View source)
{
    if (m_value_context.is_empty()) {
        return parse_css_value_in_rust(property_id, source, property_id);
    }

    auto context_guard = push_temporary_value_parsing_context(property_id);
    return parse_css_value_in_rust(property_id, source);
}

Parser::ParseErrorOr<NonnullRefPtr<StyleValue const>> Parser::parse_css_value_in_rust(PropertyID property_id, Utf16View source, Optional<PropertyID> direct_property_context)
{
    auto context = make_parse_context(ParseContextMode::Value, direct_property_context);
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

RefPtr<StyleValue const> Parser::parse_entirely_as_type(ValueType value_type)
{
    return parse_primitive_value_from_source(value_type, m_source);
}

}
