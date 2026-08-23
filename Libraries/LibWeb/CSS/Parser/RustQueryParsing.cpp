/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWeb/CSS/CSSFontFeatureValuesRule.h>
#include <LibWeb/CSS/CSSMarginRule.h>
#include <LibWeb/CSS/ContainerQuery.h>
#include <LibWeb/CSS/EnvironmentVariable.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/MediaFeatureID.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustQueryParsing.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/Serialize.h>
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

static Utf16View utf16_value(FfiSyntaxParseData const& data, size_t offset, size_t length)
{
    VERIFY(offset <= data.value_count);
    VERIFY(length <= data.value_count - offset);
    return { reinterpret_cast<char16_t const*>(data.values + offset), length };
}

static u16 resolve_query_feature(u8 kind, u16 const* code_units, size_t length)
{
    auto name = Utf16View { reinterpret_cast<char16_t const*>(code_units), length };
    if (kind == 0) {
        auto id = media_feature_id_from_string(name);
        if (!id.has_value())
            return NumericLimits<u16>::max();
        return to_underlying(*id) | (media_feature_type_is_range(*id) ? 0x100 : 0);
    }
    VERIFY(kind == 1);
    auto id = size_feature_id_from_string(name);
    if (!id.has_value())
        return NumericLimits<u16>::max();
    return to_underlying(*id) | (size_feature_type_is_range(*id) ? 0x100 : 0);
}

static FeatureComparison feature_comparison(u8 comparison)
{
    VERIFY(comparison <= to_underlying(FeatureComparison::GreaterThanOrEqual));
    return static_cast<FeatureComparison>(comparison);
}

static MatchResult match_result(u8 result)
{
    VERIFY(result <= to_underlying(MatchResult::Unknown));
    return static_cast<MatchResult>(result);
}

static FfiQueryValue const& query_value(FfiQueryParseData const& data, size_t index)
{
    VERIFY(index < data.query_value_count);
    return data.query_values[index];
}

template<typename FeatureID>
static Optional<FeatureValue> feature_value(Parser& parser, FfiQueryParseData const& data, size_t index, FeatureID id)
{
    auto const& value = query_value(data, index);
    VERIFY(value.value_type == 0);
    auto components = RustSyntaxParser::component_values(data.syntax, value.component_start, value.component_count);
    TokenStream stream { components };
    if constexpr (IsSame<FeatureID, MediaFeatureID>)
        return RustQueryParser::parse_media_feature_value(parser, id, stream);
    else
        return RustQueryParser::parse_size_feature_value(parser, id, stream);
}

enum class QueryFeatureKind : u8 {
    Media,
    Size,
};

static OwnPtr<BooleanExpression> expression(Parser&, FfiQueryParseData const&, size_t, QueryFeatureKind);

static Vector<NonnullOwnPtr<BooleanExpression>> expression_children(Parser& parser, FfiQueryParseData const& data, FfiQueryNode const& node, QueryFeatureKind kind)
{
    VERIFY(node.children_start <= data.node_index_count);
    VERIFY(node.child_count <= data.node_index_count - node.children_start);
    Vector<NonnullOwnPtr<BooleanExpression>> children;
    children.ensure_capacity(node.child_count);
    for (size_t index = 0; index < node.child_count; ++index) {
        auto child = expression(parser, data, data.node_indices[node.children_start + index], kind);
        if (!child)
            return {};
        children.unchecked_append(child.release_nonnull());
    }
    return children;
}

template<typename Feature, typename FeatureID>
static OwnPtr<BooleanExpression> query_feature(Parser& parser, FfiQueryParseData const& data, FfiQueryNode const& node)
{
    auto id = static_cast<FeatureID>(node.feature_id);
    switch (node.feature_type) {
    case 0:
        return Feature::boolean(id);
    case 1:
    case 2:
    case 3: {
        auto value = feature_value(parser, data, node.values_start, id);
        if (!value.has_value())
            return nullptr;
        if (node.feature_type == 1)
            return Feature::plain(id, value.release_value());
        if (node.feature_type == 2)
            return Feature::min(id, value.release_value());
        return Feature::max(id, value.release_value());
    }
    case 4: {
        bool has_left = node.match_result & 1;
        bool has_right = node.match_result & 2;
        VERIFY(has_left || has_right);
        auto value_index = node.values_start;
        Optional<FeatureValue> left;
        Optional<FeatureValue> right;
        if (has_left)
            left = feature_value(parser, data, value_index++, id);
        if (has_right)
            right = feature_value(parser, data, value_index, id);
        if (has_left && !left.has_value())
            return nullptr;
        if (has_right && !right.has_value())
            return nullptr;
        if (has_left && has_right)
            return Feature::range(left.release_value(), feature_comparison(node.first_comparison), id, feature_comparison(node.second_comparison), right.release_value());
        if (has_left)
            return Feature::half_range(left.release_value(), feature_comparison(node.first_comparison), id);
        return Feature::half_range(id, feature_comparison(node.first_comparison), right.release_value());
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

static Optional<PropertyNameAndID> style_feature_name(FfiQueryParseData const& data, FfiQueryNode const& node)
{
    return PropertyNameAndID::from_name(Utf16FlyString::from_utf16(utf16_value(data.syntax, node.name_offset, node.name_length)));
}

static Optional<StyleFeature::StyleRangeValue> style_range_value(FfiQueryParseData const& data, size_t index)
{
    auto const& value = query_value(data, index);
    if (value.value_type == 1) {
        auto property = PropertyNameAndID::from_name(Utf16FlyString::from_utf16(utf16_value(data.syntax, value.name_offset, value.name_length)));
        if (!property.has_value())
            return {};
        return StyleFeature::StyleRangeValue { property.release_value() };
    }
    VERIFY(value.value_type == 0);
    auto components = RustSyntaxParser::component_values(data.syntax, value.component_start, value.component_count);
    TokenStream stream { components };
    if (!Parser::parse_declaration_value_as_span(stream).has_value() || !stream.is_empty())
        return {};
    return StyleFeature::StyleRangeValue { move(components) };
}

static bool at_rule_is_supported(Utf16View name)
{
    // NB: Keep this list in sync with the C++ oracle in Parser.cpp while the verifier exists.
    if (name.equals_ignoring_ascii_case("charset"sv))
        return false;
    if (name.equals_ignoring_ascii_case("container"sv)
        || name.equals_ignoring_ascii_case("counter-style"sv)
        || name.equals_ignoring_ascii_case("font-face"sv)
        || name.equals_ignoring_ascii_case("font-feature-values"sv)
        || name.equals_ignoring_ascii_case("function"sv)
        || name.equals_ignoring_ascii_case("import"sv)
        || name.equals_ignoring_ascii_case("keyframes"sv)
        || name.equals_ignoring_ascii_case("-webkit-keyframes"sv)
        || name.equals_ignoring_ascii_case("layer"sv)
        || name.equals_ignoring_ascii_case("media"sv)
        || name.equals_ignoring_ascii_case("namespace"sv)
        || name.equals_ignoring_ascii_case("page"sv)
        || name.equals_ignoring_ascii_case("property"sv)
        || name.equals_ignoring_ascii_case("scope"sv)
        || name.equals_ignoring_ascii_case("supports"sv))
        return true;
    auto fly_string = Utf16FlyString::from_utf16(name);
    return CSSFontFeatureValuesRule::is_font_feature_value_type_at_keyword(fly_string)
        || is_margin_rule_name(fly_string);
}

static OwnPtr<BooleanExpression> expression(Parser& parser, FfiQueryParseData const& data, size_t index, QueryFeatureKind kind)
{
    VERIFY(index < data.node_count);
    auto const& node = data.nodes[index];
    switch (node.node_type) {
    case 0: {
        auto children = expression_children(parser, data, node, kind);
        if (children.size() != 1)
            return nullptr;
        return BooleanNotExpression::create(children.take_first());
    }
    case 1: {
        auto children = expression_children(parser, data, node, kind);
        if (children.size() != node.child_count)
            return nullptr;
        return BooleanAndExpression::create(move(children));
    }
    case 2: {
        auto children = expression_children(parser, data, node, kind);
        if (children.size() != node.child_count)
            return nullptr;
        return BooleanOrExpression::create(move(children));
    }
    case 3: {
        auto children = expression_children(parser, data, node, kind);
        if (children.size() != 1)
            return nullptr;
        return BooleanExpressionInParens::create(children.take_first());
    }
    case 4:
        return GeneralEnclosed::create(Utf16String::from_utf16(utf16_value(data.syntax, node.name_offset, node.name_length)), match_result(node.match_result));
    case 5:
        if (kind == QueryFeatureKind::Media)
            return query_feature<MediaFeature, MediaFeatureID>(parser, data, node);
        return query_feature<SizeFeature, SizeFeatureID>(parser, data, node);
    case 6: {
        auto const& value = query_value(data, node.values_start);
        return RustQueryParser::parse_supports_declaration(parser, RustSyntaxParser::component_values(data.syntax, value.component_start, value.component_count));
    }
    case 7: {
        auto const& value = query_value(data, node.values_start);
        return RustQueryParser::parse_supports_selector(parser, RustSyntaxParser::component_values(data.syntax, value.component_start, value.component_count));
    }
    case 8: {
        auto name = Utf16FlyString::from_utf16(utf16_value(data.syntax, node.name_offset, node.name_length));
        return Supports::FontTech::create(name, font_tech_is_supported(name));
    }
    case 9: {
        auto name = Utf16FlyString::from_utf16(utf16_value(data.syntax, node.name_offset, node.name_length));
        return Supports::FontFormat::create(name, font_format_is_supported(name));
    }
    case 10: {
        auto name = Utf16FlyString::from_utf16(utf16_value(data.syntax, node.name_offset, node.name_length));
        return Supports::AtRule::create(name, at_rule_is_supported(name));
    }
    case 11: {
        auto name = Utf16FlyString::from_utf16(utf16_value(data.syntax, node.name_offset, node.name_length));
        return Supports::Env::create(name, environment_variable_from_string(name).has_value());
    }
    case 12: {
        auto children = expression_children(parser, data, node, kind);
        if (children.size() != 1)
            return nullptr;
        return StyleQueryFunction::create(children.take_first());
    }
    case 13: {
        auto name = style_feature_name(data, node);
        if (!name.has_value())
            return nullptr;
        return StyleFeature::create_boolean(name.release_value());
    }
    case 14: {
        auto name = style_feature_name(data, node);
        if (!name.has_value())
            return nullptr;
        auto const& value = query_value(data, node.values_start);
        auto components = RustSyntaxParser::component_values(data.syntax, value.component_start, value.component_count);
        auto original_text = serialize_a_series_of_component_values_preserving_original_source_text(components);
        return StyleFeature::create_plain(name.release_value(), move(components), move(original_text));
    }
    case 15: {
        auto left = style_range_value(data, node.values_start);
        auto middle = style_range_value(data, node.values_start + 1);
        if (!left.has_value() || !middle.has_value())
            return nullptr;
        if (node.value_count == 2)
            return StyleFeature::create_range(left.release_value(), feature_comparison(node.first_comparison), middle.release_value());
        VERIFY(node.value_count == 3);
        auto right = style_range_value(data, node.values_start + 2);
        if (!right.has_value())
            return nullptr;
        return StyleFeature::create_range(left.release_value(), feature_comparison(node.first_comparison), middle.release_value(), feature_comparison(node.second_comparison), right.release_value());
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

Vector<NonnullRefPtr<MediaQuery>> RustQueryParser::parse_media_query_list(Parser& parser, Utf16View source)
{
    auto* parse = rust_parse_media_query_list(ffi_utf16_view(source), resolve_query_feature);
    if (!parse)
        return { MediaQuery::create_not_all() };
    ScopeGuard free_parse = [&] { rust_query_parse_free(parse); };
    auto data = rust_query_parse_data(parse);
    Vector<NonnullRefPtr<MediaQuery>> queries;
    queries.ensure_capacity(data.media_query_count);
    for (size_t index = 0; index < data.media_query_count; ++index) {
        auto const& ffi_query = data.media_queries[index];
        if (!ffi_query.valid) {
            queries.unchecked_append(MediaQuery::create_not_all());
            continue;
        }
        auto query = MediaQuery::create();
        query->m_negated = ffi_query.negated;
        if (ffi_query.has_media_type) {
            auto name = Utf16FlyString::from_utf16(utf16_value(data.syntax, ffi_query.media_type_offset, ffi_query.media_type_length));
            query->m_media_type = { .name = name, .known_type = media_type_from_string(name) };
        }
        if (ffi_query.has_condition) {
            auto condition = expression(parser, data, ffi_query.condition, QueryFeatureKind::Media);
            if (!condition) {
                queries.unchecked_append(MediaQuery::create_not_all());
                continue;
            }
            query->m_media_condition = move(condition);
        }
        queries.unchecked_append(move(query));
    }
    return queries;
}

RefPtr<Supports> RustQueryParser::parse_supports(Parser& parser, Utf16View source)
{
    auto* parse = rust_parse_supports_condition(ffi_utf16_view(source));
    if (!parse)
        return nullptr;
    ScopeGuard free_parse = [&] { rust_query_parse_free(parse); };
    auto data = rust_query_parse_data(parse);
    if (!data.has_root)
        return nullptr;
    parser.m_rule_context.append(RuleContext::SupportsCondition);
    ScopeGuard pop_context = [&] { parser.m_rule_context.take_last(); };
    auto condition = expression(parser, data, data.root, QueryFeatureKind::Media);
    if (!condition)
        return nullptr;
    return Supports::create(condition.release_nonnull());
}

Optional<Vector<RustQueryParser::ContainerCondition>> RustQueryParser::parse_container_condition_list(Parser& parser, Utf16View source)
{
    auto* parse = rust_parse_container_condition_list(ffi_utf16_view(source), resolve_query_feature);
    if (!parse)
        return {};
    ScopeGuard free_parse = [&] { rust_query_parse_free(parse); };
    auto data = rust_query_parse_data(parse);
    Vector<ContainerCondition> conditions;
    conditions.ensure_capacity(data.media_query_count);
    for (size_t index = 0; index < data.media_query_count; ++index) {
        auto const& ffi_condition = data.media_queries[index];
        VERIFY(ffi_condition.valid);
        Optional<Utf16FlyString> name;
        if (ffi_condition.has_media_type)
            name = Utf16FlyString::from_utf16(utf16_value(data.syntax, ffi_condition.media_type_offset, ffi_condition.media_type_length));
        RefPtr<ContainerQuery> query;
        if (ffi_condition.has_condition) {
            auto condition = expression(parser, data, ffi_condition.condition, QueryFeatureKind::Size);
            if (!condition)
                return {};
            query = ContainerQuery::create(condition.release_nonnull());
        }
        conditions.unchecked_append({ .name = move(name), .query = move(query) });
    }
    return conditions;
}

Optional<FeatureValue> RustQueryParser::parse_media_feature_value(Parser& parser, MediaFeatureID id, TokenStream<ComponentValue>& stream)
{
    return parser.parse_media_feature_value(id, stream);
}

Optional<FeatureValue> RustQueryParser::parse_size_feature_value(Parser& parser, SizeFeatureID id, TokenStream<ComponentValue>& stream)
{
    return parser.parse_size_feature_value(id, stream);
}

OwnPtr<BooleanExpression> RustQueryParser::parse_supports_declaration(Parser& parser, Vector<ComponentValue> components)
{
    TokenStream stream { components };
    auto declaration = parser.consume_a_declaration(stream, Parser::Nested::No, Parser::SaveOriginalText::Yes);
    stream.discard_whitespace();
    if (!declaration.has_value() || stream.has_next_token())
        return nullptr;
    auto text = declaration->original_full_text.release_value();
    return Supports::Declaration::create(move(text), parser.convert_to_style_property(*declaration).has_value());
}

OwnPtr<BooleanExpression> RustQueryParser::parse_supports_selector(Parser& parser, Vector<ComponentValue> components)
{
    auto selector_text = serialize_a_series_of_component_values(components);
    TokenStream stream { components };
    auto selectors = parser.parse_a_selector_list(stream, Parser::SelectorType::Standalone);
    bool matches = !selectors.is_error() && selectors.value().size() == 1
        && !selectors.value().first()->contains_unknown_webkit_pseudo_element();
    return Supports::Selector::create(move(selector_text), matches);
}

}
