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
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustQueryParsing.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/QueryValueType.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
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

Optional<Vector<RustQueryParser::SizesAttributeEntry>> RustQueryParser::split_sizes_attribute(Utf16View source)
{
    Vector<SizesAttributeEntry> entries;
    auto visit = [](void* context, u16 const* condition, size_t condition_length, u16 const* size, size_t size_length) {
        auto& entries = *static_cast<Vector<SizesAttributeEntry>*>(context);
        entries.append({
            .condition = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(condition), condition_length }),
            .size = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(size), size_length }),
        });
    };
    if (!rust_visit_sizes_attribute_entries(ffi_utf16_view(source), &entries, visit))
        return {};
    return entries;
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
    if (kind == 1) {
        auto id = size_feature_id_from_string(name);
        if (!id.has_value())
            return NumericLimits<u16>::max();
        return to_underlying(*id) | (size_feature_type_is_range(*id) ? 0x100 : 0);
    }
    VERIFY(kind == 2);
    return PropertyNameAndID::from_name(Utf16FlyString::from_utf16(name)).has_value() ? 0 : NumericLimits<u16>::max();
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
    auto source = utf16_value(data.syntax, value.source_offset, value.source_length);
    if constexpr (IsSame<FeatureID, MediaFeatureID>)
        return RustQueryParser::parse_media_feature_value(parser, id, source);
    else
        return RustQueryParser::parse_size_feature_value(parser, id, source);
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
    return StyleFeature::StyleRangeValue { Utf16String::from_utf16(utf16_value(data.syntax, value.source_offset, value.source_length)) };
}

static bool at_rule_is_supported(Utf16View name)
{
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
        return RustQueryParser::supports_declaration_feature(parser, utf16_value(data.syntax, value.source_offset, value.source_length));
    }
    case 7: {
        auto const& value = query_value(data, node.values_start);
        return RustQueryParser::supports_selector_feature(parser, utf16_value(data.syntax, value.source_offset, value.source_length));
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
            return GeneralEnclosed::create(Utf16String::from_utf16(utf16_value(data.syntax, node.name_offset, node.name_length)), MatchResult::Unknown);
        return StyleFeature::create_boolean(name.release_value());
    }
    case 14: {
        auto name = style_feature_name(data, node);
        auto const& value = query_value(data, node.values_start);
        auto original_text = Utf16String::from_utf16(utf16_value(data.syntax, value.name_offset, value.name_length));
        if (!name.has_value()) {
            return GeneralEnclosed::create(Utf16String::from_utf16(utf16_value(data.syntax, node.children_start, node.child_count)), MatchResult::Unknown);
        }
        return StyleFeature::create_plain(name.release_value(), Utf16String::from_utf16(utf16_value(data.syntax, value.source_offset, value.source_length)), move(original_text));
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

Vector<NonnullRefPtr<MediaQuery>> Parser::parse_as_media_query_list()
{
    return RustQueryParser::parse_media_query_list(*this, m_source);
}

RefPtr<MediaQuery> Parser::parse_as_media_query()
{
    auto media_query_list = parse_as_media_query_list();
    if (media_query_list.is_empty())
        return MediaQuery::create_not_all();
    if (media_query_list.size() == 1)
        return media_query_list.first();
    return nullptr;
}

static OwnPtr<BooleanExpression> adopt_root_expression(Parser& parser, FfiQueryParse* parse, QueryFeatureKind kind)
{
    if (!parse)
        return nullptr;
    ScopeGuard free_parse = [&] { rust_query_parse_free(parse); };
    auto data = rust_query_parse_data(parse);
    if (!data.has_root)
        return nullptr;
    return expression(parser, data, data.root, kind);
}

OwnPtr<BooleanExpression> RustQueryParser::parse_media_condition(Parser& parser, Utf16View source)
{
    return adopt_root_expression(parser, rust_parse_media_condition(ffi_utf16_view(source), resolve_query_feature), QueryFeatureKind::Media);
}

OwnPtr<BooleanExpression> RustQueryParser::parse_media_feature(Parser& parser, Utf16View source)
{
    return adopt_root_expression(parser, rust_parse_media_feature(ffi_utf16_view(source), resolve_query_feature), QueryFeatureKind::Media);
}

RefPtr<Supports> RustQueryParser::parse_supports(Parser& parser, Utf16View source)
{
    auto condition = parse_supports_condition(parser, source);
    if (!condition)
        return nullptr;
    return Supports::create(condition.release_nonnull());
}

OwnPtr<BooleanExpression> RustQueryParser::parse_supports_condition(Parser& parser, Utf16View source)
{
    parser.m_rule_context.append(RuleContext::SupportsCondition);
    ScopeGuard pop_context = [&] { parser.m_rule_context.take_last(); };
    return adopt_root_expression(parser, rust_parse_supports_condition(ffi_utf16_view(source)), QueryFeatureKind::Media);
}

OwnPtr<BooleanExpression> RustQueryParser::parse_supports_declaration(Parser& parser, Utf16View source)
{
    parser.m_rule_context.append(RuleContext::SupportsCondition);
    ScopeGuard pop_context = [&] { parser.m_rule_context.take_last(); };
    return adopt_root_expression(parser, rust_parse_supports_declaration(ffi_utf16_view(source)), QueryFeatureKind::Media);
}

OwnPtr<BooleanExpression> RustQueryParser::parse_style_query(Parser& parser, Utf16View source)
{
    return adopt_root_expression(parser, rust_parse_style_query(ffi_utf16_view(source), resolve_query_feature), QueryFeatureKind::Media);
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

RefPtr<StyleValue const> RustQueryParser::parse_source_size_value(Parser& parser, Utf16View source)
{
    auto keyword = ValueParserFFI::rust_parse_css_keyword_from_source(ffi_utf16_view(source));
    if (keyword == to_underlying(Keyword::Auto))
        return KeywordStyleValue::create(Keyword::Auto);

    auto parsed = parser.parse_primitive_value_from_source(ValueType::Length, source, non_negative_range);
    if (!parsed)
        return nullptr;
    if (parsed->is_calculated()) {
        auto raw_length = parsed->as_calculated().resolve_raw_length({});
        if (raw_length.has_value() && !isfinite(*raw_length))
            return nullptr;
    }
    return parsed;
}

template<typename FeatureAcceptsKeyword, typename FeatureAcceptsType>
static Optional<FeatureValue> parse_feature_value_from_source(Parser& parser, Utf16View source, FeatureAcceptsKeyword feature_accepts_keyword, FeatureAcceptsType feature_accepts_type)
{
    auto keyword = ValueParserFFI::rust_parse_css_keyword_from_source(ffi_utf16_view(source));
    if (keyword != NumericLimits<u16>::max() && feature_accepts_keyword(static_cast<Keyword>(keyword)))
        return FeatureValue(FeatureValue::Type::Ident, KeywordStyleValue::create(static_cast<Keyword>(keyword)));

    if (feature_accepts_type(QueryValueType::Boolean)) {
        if (auto integer = parser.parse_primitive_value_from_source(ValueType::Integer, source)) {
            if (integer->is_calculated() || first_is_one_of(integer->as_integer().integer(), 0, 1))
                return FeatureValue(FeatureValue::Type::Integer, integer.release_nonnull());
        }
    }

    if (feature_accepts_type(QueryValueType::Integer)) {
        if (auto integer = parser.parse_primitive_value_from_source(ValueType::Integer, source))
            return FeatureValue(FeatureValue::Type::Integer, integer.release_nonnull());
    }

    if (feature_accepts_type(QueryValueType::Length)) {
        if (auto length = parser.parse_primitive_value_from_source(ValueType::Length, source))
            return FeatureValue(FeatureValue::Type::Length, length.release_nonnull());
        if (auto number = parser.parse_primitive_value_from_source(ValueType::Number, source); number && number->is_calculated() && number->as_calculated().resolves_to_number()) {
            if (auto resolved_number = number->as_calculated().resolve_number({}); resolved_number.has_value() && *resolved_number == 0)
                return FeatureValue(FeatureValue::Type::Length, LengthStyleValue::create(Length::make_px(0)));
        }
    }

    if (feature_accepts_type(QueryValueType::Ratio)) {
        if (auto ratio = parser.parse_primitive_value_from_source(ValueType::Ratio, source))
            return FeatureValue(FeatureValue::Type::Ratio, ratio.release_nonnull());
    }

    if (feature_accepts_type(QueryValueType::Resolution)) {
        if (auto resolution = parser.parse_primitive_value_from_source(ValueType::Resolution, source))
            return FeatureValue(FeatureValue::Type::Resolution, resolution.release_nonnull());
    }

    if (source.trim_ascii_whitespace().is_empty())
        return {};
    return FeatureValue(FeatureValue::Type::Unknown, UnresolvedStyleValue::create(Utf16String::from_utf16(source), {}));
}

Optional<FeatureValue> RustQueryParser::parse_media_feature_value(Parser& parser, MediaFeatureID id, Utf16View source)
{
    auto context_guard = parser.push_temporary_value_parsing_context(SpecialContext::MediaCondition);
    return parse_feature_value_from_source(
        parser,
        source,
        [&](Keyword keyword) { return media_feature_accepts_keyword(id, keyword); },
        [&](QueryValueType type) { return media_feature_accepts_type(id, type); });
}

Optional<FeatureValue> RustQueryParser::parse_size_feature_value(Parser& parser, SizeFeatureID id, Utf16View source)
{
    auto context_guard = parser.push_temporary_value_parsing_context(SpecialContext::MediaCondition);
    return parse_feature_value_from_source(
        parser,
        source,
        [&](Keyword keyword) { return id == SizeFeatureID::Orientation && first_is_one_of(keyword, Keyword::Landscape, Keyword::Portrait); },
        [&](QueryValueType type) {
            switch (type) {
            case QueryValueType::Length:
                return first_is_one_of(id, SizeFeatureID::BlockSize, SizeFeatureID::Height, SizeFeatureID::InlineSize, SizeFeatureID::Width);
            case QueryValueType::Ratio:
                return id == SizeFeatureID::AspectRatio;
            default:
                return false;
            }
        });
}

OwnPtr<BooleanExpression> RustQueryParser::supports_declaration_feature(Parser& parser, Utf16View source)
{
    Array contexts { RuleContext::SupportsCondition };
    auto items = RustSyntaxParser::parse_block_contents(parser, source, contexts, PreservePropertySourceText::Yes);
    if (items.size() != 1 || !items.first().has<Vector<Declaration>>())
        return Supports::Declaration::create(Utf16String::from_utf16(source), false);
    auto const& declarations = items.first().get<Vector<Declaration>>();
    if (declarations.size() != 1)
        return Supports::Declaration::create(Utf16String::from_utf16(source), false);
    auto const& declaration = declarations.first();
    auto text = declaration.original_full_text.value_or(Utf16String::from_utf16(source));
    return Supports::Declaration::create(move(text), parser.convert_to_style_property(declaration).has_value());
}

OwnPtr<BooleanExpression> RustQueryParser::supports_selector_feature(Parser& parser, Utf16View source)
{
    auto selectors = parse_selector_list_in_rust(source, parser.m_declared_namespaces, false, false);
    bool matches = selectors.has_value() && selectors->size() == 1
        && !selectors->first()->contains_unknown_webkit_pseudo_element();
    return Supports::Selector::create(Utf16String::from_utf16(source), matches);
}

}
