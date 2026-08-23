/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/Utf16View.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

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

struct ParseContextStorage {
    ValueParserFFI::ParseContext context {};
};

static ParseContextStorage make_parse_context(bool in_quirks_mode, bool is_svg_presentation_attribute, ReadonlyBytes document_url, ReadonlyBytes document_base_url, size_t& random_function_index)
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

static Number::Type number_type(u8 type)
{
    VERIFY(type <= to_underlying(Number::Type::Integer));
    return static_cast<Number::Type>(type);
}

static Token token_from_component(FfiSyntaxParseData const& data, FfiSyntaxComponent const& component, bool end_token = false)
{
    auto type = end_token ? Token::Type::CloseParen : static_cast<Token::Type>(component.token_type);
    auto value = utf16_value(data, component.value_offset, component.value_length);
    auto source_offset = end_token ? component.end_source_offset : component.source_offset;
    auto source_length = end_token ? component.end_source_length : component.source_length;
    auto original_source = Utf16String::from_utf16(utf16_value(data, source_offset, source_length));
    auto fly_string = Utf16FlyString::from_utf16(value);

    Token token;
    switch (type) {
    case Token::Type::Invalid:
        VERIFY_NOT_REACHED();
    case Token::Type::Ident:
        token = Token::create_ident(move(fly_string), move(original_source));
        break;
    case Token::Type::Function:
        token = Token::create_function(move(fly_string), move(original_source));
        break;
    case Token::Type::AtKeyword:
        token = Token::create_at_keyword(move(fly_string), move(original_source));
        break;
    case Token::Type::Hash:
        token = Token::create_hash(move(fly_string), static_cast<Token::HashType>(component.hash_type), move(original_source));
        break;
    case Token::Type::String:
        token = Token::create_string(move(fly_string), move(original_source));
        break;
    case Token::Type::Url:
        token = Token::create_url(move(fly_string), move(original_source));
        break;
    case Token::Type::Delim:
        token = Token::create_delim(component.delim, move(original_source));
        break;
    case Token::Type::Number:
        token = Token::create_number(Number { number_type(component.number_type), component.number_value }, move(original_source));
        break;
    case Token::Type::Percentage:
        token = Token::create_percentage(Number { number_type(component.number_type), component.number_value }, move(original_source));
        break;
    case Token::Type::Dimension:
        token = Token::create_dimension(Number { number_type(component.number_type), component.number_value }, move(fly_string), move(original_source));
        break;
    case Token::Type::Whitespace:
        token = Token::create_whitespace(move(original_source));
        break;
    case Token::Type::EndOfFile:
    case Token::Type::BadString:
    case Token::Type::BadUrl:
    case Token::Type::CDO:
    case Token::Type::CDC:
    case Token::Type::Colon:
    case Token::Type::Semicolon:
    case Token::Type::Comma:
    case Token::Type::OpenSquare:
    case Token::Type::CloseSquare:
    case Token::Type::OpenParen:
    case Token::Type::CloseParen:
    case Token::Type::OpenCurly:
    case Token::Type::CloseCurly:
        token = Token::create(type, move(original_source));
        break;
    }
    RustSyntaxParser::set_token_position(token, source_position(component.start_line, component.start_column), source_position(component.end_line, component.end_column));
    return token;
}

static ComponentValue component_value(FfiSyntaxParseData const&, size_t);

static Vector<ComponentValue> component_values(FfiSyntaxParseData const& data, size_t start, size_t count)
{
    VERIFY(start <= data.component_index_count);
    VERIFY(count <= data.component_index_count - start);
    Vector<ComponentValue> values;
    values.ensure_capacity(count);
    for (size_t index = 0; index < count; ++index)
        values.unchecked_append(component_value(data, data.component_indices[start + index]));
    return values;
}

static ComponentValue component_value(FfiSyntaxParseData const& data, size_t index)
{
    VERIFY(index < data.component_count);
    auto const& component = data.components[index];
    if (component.component_type == 0)
        return ComponentValue { token_from_component(data, component) };

    auto children = component_values(data, component.children_start, component.child_count);
    if (component.component_type == 1) {
        auto name_token = token_from_component(data, component);
        auto end_token = token_from_component(data, component, true);
        return ComponentValue { Function {
            .name = name_token.function(),
            .value = move(children),
            .name_token = move(name_token),
            .end_token = move(end_token),
        } };
    }

    VERIFY(component.component_type == 2);
    auto opening_token = token_from_component(data, component);
    auto closing_type = opening_token.mirror_variant();
    auto end_source = Utf16String::from_utf16(utf16_value(data, component.end_source_offset, component.end_source_length));
    auto end_token = Token::create(closing_type, move(end_source));
    return ComponentValue { SimpleBlock {
        .token = opening_token,
        .value = move(children),
        .end_token = end_token,
    } };
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
    Vector<ComponentValue> values;
    if (declaration.is_property) {
        if (declaration.value_count > 0)
            values = component_values(data, declaration.values_start, declaration.value_count);
        if (declaration.property_id != NumericLimits<u16>::max())
            parsed_property_id = static_cast<PropertyID>(declaration.property_id);
        if (declaration.parse_status == FfiParseStatus::Parsed) {
            VERIFY(declaration.parsed_value);
            parsed_value = StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(declaration.parsed_value));
        } else {
            VERIFY(!declaration.parsed_value);
        }
    } else {
        values = component_values(data, declaration.values_start, declaration.value_count);
    }
    return Declaration {
        .name = Utf16FlyString::from_utf16(utf16_value(data, declaration.name_offset, declaration.name_length)),
        .value = move(values),
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
    auto prelude = component_values(data, rule.prelude_start, rule.prelude_count);
    auto prelude_text = Utf16String::from_utf16(utf16_value(data, rule.prelude_source_offset, rule.prelude_source_length));
    auto children = items(data, rule.children_start, rule.child_count);
    if (rule.rule_type == 0) {
        return AtRule {
            .name = Utf16FlyString::from_utf16(utf16_value(data, rule.name_offset, rule.name_length)),
            .prelude = move(prelude),
            .child_rules_and_lists_of_declarations = move(children),
            .is_block_rule = rule.has_block,
        };
    }

    VERIFY(rule.rule_type == 1);
    return QualifiedRule {
        .prelude = move(prelude),
        .prelude_text = move(prelude_text),
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
    auto context = make_parse_context(parser.in_quirks_mode(), parser.is_parsing_svg_presentation_attribute(), document_url, document_base_url, parser.m_random_function_index);
    auto* parse = rust_parse_css_stylesheet_syntax(ffi_utf16_view(parser.m_source), &context.context, resolve_property_id);
    VERIFY(parse);
    ScopeGuard free_parse = [&] { rust_css_syntax_parse_free(parse); };
    auto data = rust_css_syntax_parse_data(parse);
    Vector<Rule> result;
    result.ensure_capacity(data.root_count);
    for (size_t index = 0; index < data.root_count; ++index)
        result.unchecked_append(rule(data, data.roots[index]));
    return result;
}

void RustSyntaxParser::set_token_position(Token& token, SourcePosition start, SourcePosition end)
{
    token.set_position_range(Badge<RustSyntaxParser> {}, start, end);
}

Vector<RuleOrListOfDeclarations> RustSyntaxParser::parse_block_contents(Parser& parser, ReadonlySpan<RuleContext> contexts, PreservePropertySourceText preserve_property_source_text)
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
    auto context = make_parse_context(parser.in_quirks_mode(), parser.is_parsing_svg_presentation_attribute(), document_url, document_base_url, parser.m_random_function_index);
    auto* parse = rust_parse_css_block_syntax(ffi_utf16_view(parser.m_source), reinterpret_cast<u8 const*>(contexts.data()), contexts.size(), &context.context, resolve_property_id, preserve_property_source_text == PreservePropertySourceText::Yes);
    VERIFY(parse);
    ScopeGuard free_parse = [&] { rust_css_syntax_parse_free(parse); };
    auto data = rust_css_syntax_parse_data(parse);
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

}
