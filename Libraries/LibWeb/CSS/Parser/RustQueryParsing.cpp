/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWeb/CSS/ContainerQuery.h>
#include <LibWeb/CSS/MediaFeatureID.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustQueryParsing.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
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

static Optional<FeatureValue> feature_value(Parser& parser, FfiQueryParseData const& data, size_t index, MediaFeatureID id)
{
    auto const& value = query_value(data, index);
    VERIFY(value.value_type == 0);
    auto components = RustSyntaxParser::component_values(data.syntax, value.component_start, value.component_count);
    TokenStream stream { components };
    return RustQueryParser::parse_media_feature_value(parser, id, stream);
}

static OwnPtr<BooleanExpression> expression(Parser&, FfiQueryParseData const&, size_t);

static Vector<NonnullOwnPtr<BooleanExpression>> expression_children(Parser& parser, FfiQueryParseData const& data, FfiQueryNode const& node)
{
    VERIFY(node.children_start <= data.node_index_count);
    VERIFY(node.child_count <= data.node_index_count - node.children_start);
    Vector<NonnullOwnPtr<BooleanExpression>> children;
    children.ensure_capacity(node.child_count);
    for (size_t index = 0; index < node.child_count; ++index) {
        auto child = expression(parser, data, data.node_indices[node.children_start + index]);
        if (!child)
            return {};
        children.unchecked_append(child.release_nonnull());
    }
    return children;
}

static OwnPtr<BooleanExpression> media_feature(Parser& parser, FfiQueryParseData const& data, FfiQueryNode const& node)
{
    auto id = static_cast<MediaFeatureID>(node.feature_id);
    switch (node.feature_type) {
    case 0:
        return MediaFeature::boolean(id);
    case 1:
    case 2:
    case 3: {
        auto value = feature_value(parser, data, node.values_start, id);
        if (!value.has_value())
            return nullptr;
        if (node.feature_type == 1)
            return MediaFeature::plain(id, value.release_value());
        if (node.feature_type == 2)
            return MediaFeature::min(id, value.release_value());
        return MediaFeature::max(id, value.release_value());
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
            return MediaFeature::range(left.release_value(), feature_comparison(node.first_comparison), id, feature_comparison(node.second_comparison), right.release_value());
        if (has_left)
            return MediaFeature::half_range(left.release_value(), feature_comparison(node.first_comparison), id);
        return MediaFeature::half_range(id, feature_comparison(node.first_comparison), right.release_value());
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

static OwnPtr<BooleanExpression> expression(Parser& parser, FfiQueryParseData const& data, size_t index)
{
    VERIFY(index < data.node_count);
    auto const& node = data.nodes[index];
    switch (node.node_type) {
    case 0: {
        auto children = expression_children(parser, data, node);
        if (children.size() != 1)
            return nullptr;
        return BooleanNotExpression::create(children.take_first());
    }
    case 1: {
        auto children = expression_children(parser, data, node);
        if (children.size() != node.child_count)
            return nullptr;
        return BooleanAndExpression::create(move(children));
    }
    case 2: {
        auto children = expression_children(parser, data, node);
        if (children.size() != node.child_count)
            return nullptr;
        return BooleanOrExpression::create(move(children));
    }
    case 3: {
        auto children = expression_children(parser, data, node);
        if (children.size() != 1)
            return nullptr;
        return BooleanExpressionInParens::create(children.take_first());
    }
    case 4:
        return GeneralEnclosed::create(Utf16String::from_utf16(utf16_value(data.syntax, node.name_offset, node.name_length)), match_result(node.match_result));
    case 5:
        return media_feature(parser, data, node);
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
            auto condition = expression(parser, data, ffi_query.condition);
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

Optional<FeatureValue> RustQueryParser::parse_media_feature_value(Parser& parser, MediaFeatureID id, TokenStream<ComponentValue>& stream)
{
    return parser.parse_media_feature_value(id, stream);
}

}
