/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/Parser/ArbitrarySubstitutionFunctions.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustTokenizer.h>
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/Parser/TokenStream.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
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

static Parser::ParseErrorOr<void> collect_substitution_function_presence_in_rust(ReadonlySpan<ComponentValue> values, SubstitutionFunctionsPresence& presence)
{
    auto source = serialize_a_series_of_component_values(values);
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

Parser::ParseErrorOr<void> Parser::collect_arbitrary_substitution_function_presence(Vector<ComponentValue> const& component_values, SubstitutionFunctionsPresence& presence)
{
    return collect_substitution_function_presence_in_rust(component_values.span(), presence);
}

Parser::ParseErrorOr<void> Parser::collect_arbitrary_substitution_function_presence(ComponentValue const& component_value, SubstitutionFunctionsPresence& presence)
{
    return collect_substitution_function_presence_in_rust(ReadonlySpan<ComponentValue> { &component_value, 1 }, presence);
}

Optional<ArbitrarySubstitutionFunctionArguments> parse_according_to_argument_grammar(ArbitrarySubstitutionFunction function, ReadonlySpan<ComponentValue> values)
{
    auto source = serialize_a_series_of_component_values(values);
    if (!ValueParserFFI::rust_validate_arbitrary_substitution_arguments_from_source(to_underlying(function), ffi_utf16_view(source)))
        return {};

    auto split_declaration_values = [](ReadonlySpan<ComponentValue> values, Token::Type separator) {
        DeclarationValueList arguments;
        for (size_t index = 0; index < values.size(); ++index) {
            if (!values[index].is(separator))
                continue;
            arguments.append(values.slice(0, index));
            arguments.append(values.slice(index + 1));
            return arguments;
        }
        arguments.append(values);
        return arguments;
    };

    if (function != ArbitrarySubstitutionFunction::DashedFunction && function != ArbitrarySubstitutionFunction::If)
        return split_declaration_values(values, Token::Type::Comma);

    if (function == ArbitrarySubstitutionFunction::If) {
        IfArgs branches;
        size_t branch_start = 0;
        while (branch_start < values.size()) {
            size_t branch_end = branch_start;
            while (branch_end < values.size() && !values[branch_end].is(Token::Type::Semicolon))
                ++branch_end;
            auto branch = split_declaration_values(values.slice(branch_start, branch_end - branch_start), Token::Type::Colon);
            Optional<ReadonlySpan<ComponentValue>> value;
            if (branch.size() == 2)
                value = branch[1];
            branches.append({ branch[0], value });
            branch_start = branch_end + 1;
        }
        return branches;
    }

    DeclarationValueList arguments;
    size_t argument_start = 0;
    while (argument_start < values.size() && values[argument_start].is(Token::Type::Whitespace))
        ++argument_start;
    while (argument_start < values.size()) {
        size_t argument_end = argument_start;
        while (argument_end < values.size() && !values[argument_end].is(Token::Type::Comma))
            ++argument_end;
        auto argument = values.slice(argument_start, argument_end - argument_start);
        size_t content_end = argument.size();
        while (content_end > 0 && argument[content_end - 1].is(Token::Type::Whitespace))
            --content_end;
        if (content_end == 1 && argument[0].is_block() && argument[0].block().is_curly()) {
            auto block_values = argument[0].block().value.span();
            size_t block_start = 0;
            while (block_start < block_values.size() && block_values[block_start].is(Token::Type::Whitespace))
                ++block_start;
            arguments.append(block_values.slice(block_start));
        } else {
            arguments.append(argument);
        }
        argument_start = argument_end + 1;
        while (argument_start < values.size() && values[argument_start].is(Token::Type::Whitespace))
            ++argument_start;
    }
    return arguments;
}

static bool contains_arbitrary_substitution_function(ReadonlySpan<ComponentValue> values)
{
    for (auto const& value : values) {
        if (value.is_function()) {
            if (to_arbitrary_substitution_function(value.function().name).has_value()
                || contains_arbitrary_substitution_function(value.function().value.span()))
                return true;
        } else if (value.is_block() && contains_arbitrary_substitution_function(value.block().value.span())) {
            return true;
        }
    }
    return false;
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
    auto syntax = ValueParserFFI::rust_parse_syntax_component(ffi_utf16_view(source), limit_single_component_ident_to_custom_ident == LimitSingleComponentIdentToCustomIdent::Yes);
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

    if (any_of(component_values, [](auto const& component_value) { return component_value.is(Token::Type::Invalid); }))
        return nullptr;

    auto source = serialize_a_series_of_component_values_preserving_original_source_text(component_values);
    auto syntax = ValueParserFFI::rust_parse_syntax(ffi_utf16_view(source), limit_single_component_ident_to_custom_ident == LimitSingleComponentIdentToCustomIdent::Yes);
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
    auto source = serialize_a_series_of_component_values_preserving_original_source_text(input);
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

Parser::ParseErrorOr<NonnullRefPtr<StyleValue const>> Parser::parse_css_value(PropertyID property_id, TokenStream<ComponentValue>& tokens, Optional<Utf16String> original_source_text, ValueIsSubstituted value_is_substituted)
{
    auto context_guard = push_temporary_value_parsing_context(property_id);

    Utf16String unresolved_source;
    Utf16String comparison_source;
    bool contains_attr_tainted_values = false;
    bool const needs_unresolved_source = property_id == PropertyID::Custom
        || contains_arbitrary_substitution_function(tokens.remaining_tokens());
    if (original_source_text.has_value()) {
        if (needs_unresolved_source) {
            unresolved_source = *original_source_text;
            comparison_source = serialize_a_series_of_component_values(tokens.remaining_tokens())
                                    .trim_ascii_whitespace();
        }
    } else {
        for (auto const& token : tokens.remaining_tokens()) {
            if (token.contains_guaranteed_invalid_value())
                return ParseError::SyntaxError;
            contains_attr_tainted_values |= token.contains_attr_tainted_value();
        }
        if (needs_unresolved_source)
            unresolved_source = serialize_a_series_of_component_values_preserving_original_source_text(tokens.remaining_tokens()).trim_ascii_whitespace();
    }

    auto const remaining_tokens = tokens.remaining_tokens();
    auto source = original_source_text.has_value()
        ? *original_source_text
        : serialize_a_series_of_component_values_for_retokenization(remaining_tokens);
    auto parsed_value = TRY(parse_css_value_in_rust(property_id, source, unresolved_source, comparison_source, contains_attr_tainted_values, value_is_substituted));
    while (tokens.has_next_token())
        tokens.discard_a_token();
    return parsed_value;
}

Parser::ParseErrorOr<NonnullRefPtr<StyleValue const>> Parser::parse_css_value_from_source(PropertyID property_id, Utf16View source)
{
    bool needs_serialized_source = false;
    bool needs_document_urls = false;
    if (m_value_context.is_empty()) {
        auto parsed_value = parse_css_value_in_rust(property_id, source, {}, {}, false, ValueIsSubstituted::No, true, !!m_document, &needs_serialized_source, &needs_document_urls, property_id);
        if (!needs_serialized_source && !needs_document_urls)
            return parsed_value;

        if (needs_document_urls) {
            parsed_value = parse_css_value_in_rust(property_id, source, {}, {}, false, ValueIsSubstituted::No, true, false, &needs_serialized_source, nullptr, property_id);
            if (!needs_serialized_source)
                return parsed_value;
        }
    } else {
        auto context_guard = push_temporary_value_parsing_context(property_id);
        auto parsed_value = parse_css_value_in_rust(property_id, source, {}, {}, false, ValueIsSubstituted::No, true, false, &needs_serialized_source);
        if (!needs_serialized_source)
            return parsed_value;
    }

    auto source_tokens = RustTokenizer::tokenize(source);
    TokenStream source_token_stream { source_tokens };
    auto component_values = parse_a_list_of_component_values(source_token_stream);
    auto tokens = TokenStream(component_values);
    return parse_css_value(property_id, tokens);
}

Parser::ParseErrorOr<NonnullRefPtr<StyleValue const>> Parser::parse_css_value_in_rust(PropertyID property_id, Utf16View source, Utf16View unresolved_source, Utf16View comparison_source, bool contains_attr_tainted_values, ValueIsSubstituted value_is_substituted, bool retry_with_serialized_source, bool retry_without_document_urls, bool* needs_serialized_source, bool* needs_document_urls, Optional<PropertyID> direct_property_context)
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
    if (m_document && !retry_without_document_urls) {
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
        .is_substituted_value = value_is_substituted == ValueIsSubstituted::Yes,
        .contains_attr_tainted_values = contains_attr_tainted_values,
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
        .random_function_index = &m_random_function_index,
    };
    ValueParserFFI::FfiParseStatus status { ValueParserFFI::FfiParseStatus::NotHandled };
    u8 const* reason { nullptr };
    auto const* parsed_value = ValueParserFFI::rust_parse_css_value(
        &context, to_underlying(property_id), ffi_utf16_view(source),
        ffi_utf16_view(unresolved_source), ffi_utf16_view(comparison_source), retry_with_serialized_source, retry_without_document_urls, &status, &reason);
    if (needs_serialized_source)
        *needs_serialized_source = status == ValueParserFFI::FfiParseStatus::NeedsSerializedSource;
    if (needs_document_urls)
        *needs_document_urls = status == ValueParserFFI::FfiParseStatus::NeedsDocumentUrls;

    if (status != ValueParserFFI::FfiParseStatus::Parsed) {
        if (status == ValueParserFFI::FfiParseStatus::NotHandled)
            warnln("Rust CSS value parser did not handle property {}", string_from_property_id(property_id));
        return ParseError::SyntaxError;
    }

    VERIFY(parsed_value);
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(parsed_value));
}

Optional<RefPtr<StyleValue const>> Parser::parse_font_descriptor_value_in_rust(FontDescriptorKind kind, TokenStream<ComponentValue>& tokens)
{
    auto source = kind == FontDescriptorKind::SourceList
        ? serialize_a_series_of_component_values_for_retokenization(tokens.remaining_tokens())
        : serialize_a_series_of_component_values_preserving_original_source_text(tokens.remaining_tokens());
    if (kind == FontDescriptorKind::UnicodeRangeList)
        source = source.replace("/**/"_utf16, ""_utf16, ReplaceMode::All);

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
        .random_function_index = &m_random_function_index,
    };
    ValueParserFFI::FfiParseStatus status { ValueParserFFI::FfiParseStatus::NotHandled };
    void const* parsed_value = nullptr;
    parsed_value = ValueParserFFI::rust_parse_font_descriptor(
        &context, static_cast<ValueParserFFI::FfiFontDescriptorKind>(kind), ffi_utf16_view(source), &status);
    if (status == ValueParserFFI::FfiParseStatus::NotHandled) {
        warnln("Rust CSS value parser did not handle a font descriptor");
        return RefPtr<StyleValue const> {};
    }
    if (status == ValueParserFFI::FfiParseStatus::Invalid)
        return RefPtr<StyleValue const> {};

    VERIFY(parsed_value);
    while (tokens.has_next_token())
        tokens.discard_a_token();
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(parsed_value));
}

RefPtr<StyleValue const> Parser::parse_primitive_value(ValueType value_type, TokenStream<ComponentValue>& tokens, NumericRange const& accepted_range)
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
        .random_function_index = &m_random_function_index,
    };
    auto source = serialize_a_series_of_component_values_for_retokenization(tokens.remaining_tokens());
    size_t consumed = 0;
    auto const* parsed = ValueParserFFI::rust_parse_css_primitive_from_source(
        &context, to_underlying(value_type), ffi_utf16_view(source),
        accepted_range.min, accepted_range.max, &consumed);
    if (!parsed)
        return nullptr;
    for (size_t index = 0; index < consumed; ++index)
        tokens.discard_a_token();
    return StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(parsed));
}

RefPtr<StyleValue const> Parser::parse_value(ValueType value_type, TokenStream<ComponentValue>& tokens)
{
    return parse_primitive_value(value_type, tokens);
}

RefPtr<StyleValue const> Parser::parse_entirely_as_type(ValueType value_type)
{
    auto values = parse_a_list_of_component_values(token_stream());
    TokenStream tokens { values };
    auto parsed = parse_primitive_value(value_type, tokens);
    tokens.discard_whitespace();
    return parsed && !tokens.has_next_token() ? parsed : nullptr;
}

RefPtr<StyleValue const> Parser::parse_integer_value(TokenStream<ComponentValue>& tokens, NumericRange const& accepted_range)
{
    return parse_primitive_value(ValueType::Integer, tokens, accepted_range);
}

RefPtr<StyleValue const> Parser::parse_length_value(TokenStream<ComponentValue>& tokens, NumericRange const& accepted_range)
{
    return parse_primitive_value(ValueType::Length, tokens, accepted_range);
}

RefPtr<StyleValue const> Parser::parse_number_value(TokenStream<ComponentValue>& tokens, NumericRange const& accepted_range)
{
    return parse_primitive_value(ValueType::Number, tokens, accepted_range);
}

RefPtr<StyleValue const> Parser::parse_percentage_value(TokenStream<ComponentValue>& tokens, NumericRange const& accepted_range)
{
    return parse_primitive_value(ValueType::Percentage, tokens, accepted_range);
}

RefPtr<StyleValue const> Parser::parse_resolution_value(TokenStream<ComponentValue>& tokens, NumericRange const& accepted_range)
{
    return parse_primitive_value(ValueType::Resolution, tokens, accepted_range);
}

RefPtr<StyleValue const> Parser::parse_ratio_value(TokenStream<ComponentValue>& tokens)
{
    return parse_primitive_value(ValueType::Ratio, tokens);
}

RefPtr<StringStyleValue const> Parser::parse_string_value(TokenStream<ComponentValue>& tokens)
{
    auto value = parse_primitive_value(ValueType::String, tokens);
    if (!value)
        return nullptr;
    return &value->as_string();
}

RefPtr<StyleValue const> Parser::parse_color_value(TokenStream<ComponentValue>& tokens)
{
    return parse_primitive_value(ValueType::Color, tokens);
}

RefPtr<URLStyleValue const> Parser::parse_url_value(TokenStream<ComponentValue>& tokens)
{
    auto value = parse_primitive_value(ValueType::Url, tokens);
    return value ? &value->as_url() : nullptr;
}

Optional<URL> Parser::parse_url_function(TokenStream<ComponentValue>& tokens)
{
    auto value = parse_url_value(tokens);
    return value ? Optional<URL> { value->url() } : OptionalNone {};
}

Optional<Utf16FlyString> Parser::parse_custom_ident(TokenStream<ComponentValue>& tokens, ReadonlySpan<Utf16View> blacklist)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto const& token = tokens.next_token();
    auto keyword = token.is(Token::Type::Ident) ? keyword_from_string(token.token().ident()) : Optional<Keyword> {};
    if (!token.is(Token::Type::Ident) || (keyword.has_value() && is_css_wide_keyword(*keyword))
        || token.token().ident().equals_ignoring_ascii_case("default"sv)
        || any_of(blacklist, [&](auto blocked) { return token.token().ident().equals_ignoring_ascii_case(blocked); }))
        return {};
    auto ident = token.token().ident();
    tokens.discard_a_token();
    transaction.commit();
    return ident;
}

RefPtr<CustomIdentStyleValue const> Parser::parse_custom_ident_value(TokenStream<ComponentValue>& tokens, ReadonlySpan<Utf16View> blacklist)
{
    auto ident = parse_custom_ident(tokens, blacklist);
    if (!ident.has_value())
        return nullptr;
    return CustomIdentStyleValue::create(ident.release_value());
}

Optional<Utf16FlyString> Parser::parse_dashed_ident(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto ident = parse_custom_ident(tokens, {});
    if (!ident.has_value() || !ident->starts_with("--"sv))
        return {};
    transaction.commit();
    return ident;
}

RefPtr<StyleValue const> Parser::parse_keyword_value(TokenStream<ComponentValue>& tokens)
{
    tokens.discard_whitespace();
    auto const& token = tokens.next_token();
    if (!token.is(Token::Type::Ident))
        return nullptr;
    auto keyword = keyword_from_string(token.token().ident());
    if (!keyword.has_value())
        return nullptr;
    tokens.discard_a_token();
    return KeywordStyleValue::create(*keyword);
}

RefPtr<StyleValue const> Parser::parse_specific_keyword_value(TokenStream<ComponentValue>& tokens, ReadonlySpan<Keyword> accepted_keywords)
{
    auto transaction = tokens.begin_transaction();
    auto value = parse_keyword_value(tokens);
    if (!value || !accepted_keywords.contains_slow(value->to_keyword()))
        return nullptr;
    transaction.commit();
    return value;
}

RefPtr<StyleValue const> Parser::parse_all_as_single_keyword_value(TokenStream<ComponentValue>& tokens, Keyword keyword)
{
    auto transaction = tokens.begin_transaction();
    auto value = parse_specific_keyword_value(tokens, { &keyword, 1 });
    tokens.discard_whitespace();
    if (!value || tokens.has_next_token())
        return nullptr;
    transaction.commit();
    return value;
}

RefPtr<StyleValueList const> Parser::parse_comma_separated_value_list(TokenStream<ComponentValue>& tokens, ParseFunction parse_one_value)
{
    tokens.discard_whitespace();
    auto first = parse_one_value(tokens);
    tokens.discard_whitespace();
    if (!first)
        return nullptr;
    StyleValueVector values { first.release_nonnull() };
    while (tokens.has_next_token()) {
        if (!tokens.consume_a_token().is(Token::Type::Comma))
            return nullptr;
        tokens.discard_whitespace();
        auto value = parse_one_value(tokens);
        if (!value)
            return nullptr;
        values.append(value.release_nonnull());
        tokens.discard_whitespace();
    }
    return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
}

RefPtr<StyleValue const> Parser::parse_family_name_value(TokenStream<ComponentValue>& tokens)
{
    return parse_font_descriptor_value_in_rust(FontDescriptorKind::FamilyName, tokens).value_or(nullptr);
}

Optional<Utf16FlyString> Parser::parse_counter_style_name(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto custom_ident = parse_custom_ident(tokens, { { "none"sv } });
    if (!custom_ident.has_value())
        return {};
    auto keyword = keyword_from_string(*custom_ident);
    if (keyword.has_value() && keyword_to_counter_style_name_keyword(*keyword).has_value())
        custom_ident = custom_ident->to_ascii_lowercase();
    transaction.commit();
    return custom_ident;
}

RefPtr<StyleValue const> Parser::parse_nonnegative_integer_symbol_pair_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    RefPtr<StyleValue const> integer;
    RefPtr<StyleValue const> symbol;
    while (tokens.has_next_token()) {
        if (auto value = parse_integer_value(tokens, non_negative_integer_range)) {
            if (integer)
                return nullptr;
            integer = value;
        } else if (auto value = parse_symbol_value(tokens)) {
            if (symbol)
                return nullptr;
            symbol = value;
        } else {
            break;
        }
        tokens.discard_whitespace();
    }
    if (!integer || !symbol)
        return nullptr;
    transaction.commit();
    return StyleValueList::create({ integer.release_nonnull(), symbol.release_nonnull() }, StyleValueList::Separator::Space);
}

}
