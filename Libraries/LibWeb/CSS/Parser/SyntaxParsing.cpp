/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/ScopeGuard.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/CSS/Parser/ArbitrarySubstitutionFunctions.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/Parser/TokenStream.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/DOM/Document.h>
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

static Utf16String source_text_from_component_values(ReadonlySpan<ComponentValue> values)
{
    Utf16StringBuilder builder;
    for (auto const& value : values) {
        auto source = value.original_source_text();
        if (source.is_empty())
            return serialize_a_series_of_component_values(values);
        builder.append(source);
    }
    return builder.to_string();
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

RefPtr<SyntaxNode> parse_syntax_component(TokenStream<ComponentValue>& tokens, LimitSingleComponentIdentToCustomIdent limit_single_component_ident_to_custom_ident)
{
    auto transaction = tokens.begin_transaction();
    auto source = serialize_a_series_of_component_values_preserving_original_source_text(tokens.remaining_tokens());
    auto syntax = ValueParserFFI::rust_parse_syntax_component(source.bytes().data(), source.bytes().size(), limit_single_component_ident_to_custom_ident == LimitSingleComponentIdentToCustomIdent::Yes);
    if (!syntax)
        return nullptr;
    ScopeGuard free_syntax = [&] { ValueParserFFI::rust_syntax_free(syntax); };
    auto consumed_component_values = ValueParserFFI::rust_syntax_consumed_component_values(syntax);
    for (size_t index = 0; index < consumed_component_values; ++index)
        tokens.discard_a_token();
    transaction.commit();
    return syntax_node_from_rust(syntax, ValueParserFFI::rust_syntax_root(syntax));
}

// https://drafts.csswg.org/css-values-5/#typedef-syntax
RefPtr<SyntaxNode> parse_as_syntax(Vector<ComponentValue> const& component_values, LimitSingleComponentIdentToCustomIdent limit_single_component_ident_to_custom_ident)
{
    // <syntax> = '*' | <syntax-component> [ <syntax-combinator> <syntax-component> ]* | <syntax-string>
    // <syntax-component> = <syntax-single-component> <syntax-multiplier>?
    //                    | '<' transform-list '>'
    // <syntax-single-component> = '<' <syntax-type-name> '>' | <ident>
    // <syntax-type-name> = angle | color | custom-ident | image | integer
    //                    | length | length-percentage | number
    //                    | percentage | resolution | string | time
    //                    | url | transform-function
    // <syntax-combinator> = '|'
    // <syntax-multiplier> = [ '#' | '+' ]
    //
    // <syntax-string> = <string>
    // FIXME: Eventually, extend this to also parse *any* CSS grammar, not just for the <syntax> type.

    auto source = serialize_a_series_of_component_values_preserving_original_source_text(component_values);
    auto syntax = ValueParserFFI::rust_parse_syntax(source.bytes().data(), source.bytes().size(), limit_single_component_ident_to_custom_ident == LimitSingleComponentIdentToCustomIdent::Yes);
    if (!syntax)
        return nullptr;
    ScopeGuard free_syntax = [&] { ValueParserFFI::rust_syntax_free(syntax); };
    return syntax_node_from_rust(syntax, ValueParserFFI::rust_syntax_root(syntax));
}

NonnullRefPtr<StyleValue const> parse_with_a_syntax(ParsingParams const& parsing_params, Vector<ComponentValue> const& input, SyntaxNode const& syntax)
{
    return Parser::create(parsing_params, ""sv).parse_with_a_syntax(input, syntax);
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
NonnullRefPtr<StyleValue const> Parser::parse_with_a_syntax(Vector<ComponentValue> const& input, SyntaxNode const& syntax)
{
    auto source = source_text_from_component_values(input);
    auto serialized_syntax = syntax.to_string();

    Vector<ValueParserFFI::FfiValueParsingContext, 1> value_contexts;
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
            },
            [&](RelativeColorParseContext const& relative_color_context) {
                static_assert(relative_color_context.allowed_channels.size() <= 64);
                ffi_context.kind = ValueParserFFI::FfiValueParsingContextKind::RelativeColor;
                for (size_t i = 0; i < relative_color_context.allowed_channels.size(); ++i) {
                    if (relative_color_context.allowed_channels[i])
                        ffi_context.allowed_channels |= static_cast<u64>(1) << i;
                }
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
    auto retain_utf16_fly_string = [](u16 const* code_units, size_t length) -> size_t {
        return Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(code_units), length }).to_raw_leaked();
    };
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
        .normalize_svg_path_data = nullptr,
        .font_format_is_supported = nullptr,
        .font_tech_is_supported = nullptr,
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

}
