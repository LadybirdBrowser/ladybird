/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustTokenizer.h>
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

static ValueParserFFI::FfiUtf16View ffi_utf16_view(Utf16View view)
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

static RefPtr<SyntaxNode> syntax_node_from_rust(void const* syntax, size_t node_index)
{
    auto node_type = static_cast<SyntaxNode::NodeType>(ValueParserFFI::rust_syntax_node_type(syntax, node_index));
    switch (node_type) {
    case SyntaxNode::NodeType::Universal:
        return UniversalSyntaxNode::create();
    case SyntaxNode::NodeType::Ident:
    case SyntaxNode::NodeType::Type: {
        size_t value_length = 0;
        auto value = ValueParserFFI::rust_syntax_node_value(syntax, node_index, &value_length);
        auto fly_string = Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(value), value_length });
        if (node_type == SyntaxNode::NodeType::Type)
            return TypeSyntaxNode::create(move(fly_string));
        auto case_sensitivity = ValueParserFFI::rust_syntax_node_is_case_sensitive(syntax, node_index) ? CaseSensitivity::CaseSensitive : CaseSensitivity::CaseInsensitive;
        return IdentSyntaxNode::create(move(fly_string), case_sensitivity);
    }
    case SyntaxNode::NodeType::Multiplier:
    case SyntaxNode::NodeType::CommaSeparatedMultiplier: {
        auto child = syntax_node_from_rust(syntax, ValueParserFFI::rust_syntax_node_child(syntax, node_index, 0));
        VERIFY(child);
        if (node_type == SyntaxNode::NodeType::Multiplier)
            return MultiplierSyntaxNode::create(child.release_nonnull());
        return CommaSeparatedMultiplierSyntaxNode::create(child.release_nonnull());
    }
    case SyntaxNode::NodeType::Alternatives: {
        Vector<NonnullRefPtr<SyntaxNode>> children;
        auto child_count = ValueParserFFI::rust_syntax_node_child_count(syntax, node_index);
        children.ensure_capacity(child_count);
        for (size_t index = 0; index < child_count; ++index) {
            auto child = syntax_node_from_rust(syntax, ValueParserFFI::rust_syntax_node_child(syntax, node_index, index));
            VERIFY(child);
            children.unchecked_append(child.release_nonnull());
        }
        return AlternativesSyntaxNode::create(move(children));
    }
    }
    VERIFY_NOT_REACHED();
}

RefPtr<SyntaxNode> parse_as_syntax(Utf16View source, LimitSingleComponentIdentToCustomIdent limit_single_component_ident_to_custom_ident)
{
    auto normalized_source = RustTokenizer::normalize_input(source);
    auto syntax = ValueParserFFI::rust_parse_syntax(ffi_utf16_view(normalized_source), limit_single_component_ident_to_custom_ident == LimitSingleComponentIdentToCustomIdent::Yes);
    if (!syntax)
        return nullptr;
    ScopeGuard free_syntax = [&] { ValueParserFFI::rust_syntax_free(syntax); };
    return syntax_node_from_rust(syntax, ValueParserFFI::rust_syntax_root(syntax));
}

NonnullRefPtr<StyleValue const> parse_with_a_syntax(ParsingParams const& parsing_params, Utf16View input, SyntaxNode const& syntax)
{
    return Parser::create(parsing_params, input).parse_with_a_syntax(syntax);
}

static bool syntax_contains_case_sensitive_identifier(SyntaxNode const& syntax)
{
    switch (syntax.type()) {
    case SyntaxNode::NodeType::Universal:
    case SyntaxNode::NodeType::Type:
        return false;
    case SyntaxNode::NodeType::Ident:
        return as<IdentSyntaxNode>(syntax).case_sensitivity() == CaseSensitivity::CaseSensitive;
    case SyntaxNode::NodeType::Multiplier:
        return syntax_contains_case_sensitive_identifier(as<MultiplierSyntaxNode>(syntax).child());
    case SyntaxNode::NodeType::CommaSeparatedMultiplier:
        return syntax_contains_case_sensitive_identifier(as<CommaSeparatedMultiplierSyntaxNode>(syntax).child());
    case SyntaxNode::NodeType::Alternatives:
        return any_of(as<AlternativesSyntaxNode>(syntax).children(), [](auto const& child) {
            return syntax_contains_case_sensitive_identifier(*child);
        });
    }
    VERIFY_NOT_REACHED();
}

// https://drafts.csswg.org/css-values-5/#parse-with-a-syntax
NonnullRefPtr<StyleValue const> Parser::parse_with_a_syntax(Utf16View source, SyntaxNode const& syntax)
{
    auto serialized_syntax = syntax.to_string();

    Vector<ValueParserFFI::FfiValueParsingContext, 1> value_contexts;
    ValueParserFFI::FfiValueParsingContext single_property_context {};
    ValueParserFFI::FfiValueParsingContext const* value_context_data;
    size_t value_context_count;
    if (m_value_context.size() == 1 && m_value_context[0].has<PropertyID>()) {
        single_property_context.kind = ValueParserFFI::FfiValueParsingContextKind::Property;
        single_property_context.value = to_underlying(m_value_context[0].get<PropertyID>());
        value_context_data = &single_property_context;
        value_context_count = 1;
    } else {
        value_contexts.ensure_capacity(m_value_context.size());
        for (auto const& value_context : m_value_context) {
            ValueParserFFI::FfiValueParsingContext ffi_context {};
            value_context.visit(
                [&](PropertyID context_property_id) {
                    ffi_context.kind = ValueParserFFI::FfiValueParsingContextKind::Property;
                    ffi_context.value = to_underlying(context_property_id);
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
        value_context_data = value_contexts.data();
        value_context_count = value_contexts.size();
    }

    ReadonlyBytes document_url;
    ReadonlyBytes document_base_url;
    if (m_document) {
        if (!m_serialized_document_url.has_value())
            m_serialized_document_url = m_document->url().serialize();
        if (!m_serialized_document_base_url.has_value())
            m_serialized_document_base_url = m_document->base_url().serialize();
        document_url = m_serialized_document_url->bytes();
        document_base_url = m_serialized_document_base_url->bytes();
    }
    auto retain_utf16_fly_string = [](u16 const* code_units, size_t length) -> size_t {
        return Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(code_units), length }).to_raw_leaked();
    };
    ValueParserFFI::ParseContext context {
        .in_quirks_mode = in_quirks_mode(),
        .is_svg_presentation_attribute = is_parsing_svg_presentation_attribute(),
        .is_substituted_value = false,
        .contains_attr_tainted_values = false,
        .value_contexts = value_context_data,
        .value_context_count = value_context_count,
        .document_url = document_url.data(),
        .document_url_length = document_url.size(),
        .document_base_url = document_base_url.data(),
        .document_base_url_length = document_base_url.size(),
        .intern_utf16_fly_string = retain_utf16_fly_string,
        .normalize_svg_path_data = nullptr,
        .precomputed_svg_paths = nullptr,
        .precomputed_svg_path_count = 0,
        .font_format_is_supported = nullptr,
        .font_tech_is_supported = nullptr,
        .descriptor_integer_resolution_context = nullptr,
        .resolve_descriptor_integer = nullptr,
        .random_function_index = &m_random_function_index,
    };
    ValueParserFFI::FfiParseStatus status { ValueParserFFI::FfiParseStatus::Invalid };
    auto parsed = ValueParserFFI::rust_parse_with_syntax(
        &context,
        ffi_utf16_view(source), ffi_utf16_view(serialized_syntax),
        syntax_contains_case_sensitive_identifier(syntax), &status);
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
    Vector<ValueParserFFI::FfiValueParsingContext, 1> value_contexts;
    ValueParserFFI::FfiValueParsingContext single_property_context {};
    ValueParserFFI::FfiValueParsingContext const* value_context_data;
    size_t value_context_count;
    if (direct_property_context.has_value()) {
        VERIFY(m_value_context.is_empty());
        single_property_context.kind = ValueParserFFI::FfiValueParsingContextKind::Property;
        single_property_context.value = to_underlying(*direct_property_context);
        value_context_data = &single_property_context;
        value_context_count = 1;
    } else if (m_value_context.size() == 1 && m_value_context[0].has<PropertyID>()) {
        single_property_context.kind = ValueParserFFI::FfiValueParsingContextKind::Property;
        single_property_context.value = to_underlying(m_value_context[0].get<PropertyID>());
        value_context_data = &single_property_context;
        value_context_count = 1;
    } else {
        value_contexts.ensure_capacity(m_value_context.size());
        for (auto const& value_context : m_value_context) {
            ValueParserFFI::FfiValueParsingContext ffi_context {};
            value_context.visit(
                [&](PropertyID context_property_id) {
                    ffi_context.kind = ValueParserFFI::FfiValueParsingContextKind::Property;
                    ffi_context.value = to_underlying(context_property_id);
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
        value_context_data = value_contexts.data();
        value_context_count = value_contexts.size();
    }

    ReadonlyBytes document_url;
    ReadonlyBytes document_base_url;
    if (m_document) {
        if (!m_serialized_document_url.has_value())
            m_serialized_document_url = m_document->url().serialize();
        if (!m_serialized_document_base_url.has_value())
            m_serialized_document_base_url = m_document->base_url().serialize();
        document_url = m_serialized_document_url->bytes();
        document_base_url = m_serialized_document_base_url->bytes();
    }
    ValueParserFFI::ParseContext context {
        .in_quirks_mode = in_quirks_mode(),
        .is_svg_presentation_attribute = is_parsing_svg_presentation_attribute(),
        .is_substituted_value = false,
        .contains_attr_tainted_values = false,
        .value_contexts = value_context_data,
        .value_context_count = value_context_count,
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
        .descriptor_integer_resolution_context = nullptr,
        .resolve_descriptor_integer = nullptr,
        .random_function_index = &m_random_function_index,
    };
    ValueParserFFI::FfiParseStatus status { ValueParserFFI::FfiParseStatus::NotHandled };
    auto const* parsed_value = ValueParserFFI::rust_parse_css_value(
        &context, to_underlying(property_id), ffi_utf16_view(source), &status);

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
    Vector<ValueParserFFI::FfiValueParsingContext, 1> value_contexts;
    value_contexts.ensure_capacity(m_value_context.size());
    for (auto const& value_context : m_value_context) {
        ValueParserFFI::FfiValueParsingContext ffi_context {};
        value_context.visit(
            [&](PropertyID property) {
                ffi_context.kind = ValueParserFFI::FfiValueParsingContextKind::Property;
                ffi_context.value = to_underlying(property);
            },
            [&](FunctionContext const& function) {
                ffi_context.kind = ValueParserFFI::FfiValueParsingContextKind::Function;
                ffi_context.name = ffi_utf16_view(function.name);
            },
            [&](DescriptorContext const& descriptor) {
                ffi_context.kind = ValueParserFFI::FfiValueParsingContextKind::Descriptor;
                ffi_context.value = to_underlying(descriptor.at_rule);
                ffi_context.secondary_value = to_underlying(descriptor.descriptor);
            },
            [&](SpecialContext special) {
                ffi_context.kind = ValueParserFFI::FfiValueParsingContextKind::Special;
                ffi_context.value = to_underlying(special);
            });
        value_contexts.append(ffi_context);
    }
    ReadonlyBytes document_url;
    ReadonlyBytes document_base_url;
    if (m_document) {
        if (!m_serialized_document_url.has_value())
            m_serialized_document_url = m_document->url().serialize();
        if (!m_serialized_document_base_url.has_value())
            m_serialized_document_base_url = m_document->base_url().serialize();
        document_url = m_serialized_document_url->bytes();
        document_base_url = m_serialized_document_base_url->bytes();
    }
    ValueParserFFI::ParseContext context {
        .in_quirks_mode = in_quirks_mode(),
        .is_svg_presentation_attribute = is_parsing_svg_presentation_attribute(),
        .is_substituted_value = false,
        .contains_attr_tainted_values = false,
        .value_contexts = value_contexts.data(),
        .value_context_count = value_contexts.size(),
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
        .descriptor_integer_resolution_context = nullptr,
        .resolve_descriptor_integer = nullptr,
        .random_function_index = &m_random_function_index,
    };
    auto const* parsed = ValueParserFFI::rust_parse_entire_css_primitive_from_source(
        &context, to_underlying(value_type), ffi_utf16_view(source), accepted_range.min, accepted_range.max);
    if (!parsed)
        return nullptr;
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(parsed));
}

RefPtr<StyleValue const> Parser::parse_entirely_as_type(ValueType value_type)
{
    return parse_primitive_value_from_source(value_type, m_source);
}

}
