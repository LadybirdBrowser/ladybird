/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ContainerQuery.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustQueryParsing.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

using namespace ValueParserFFI;

Vector<NonnullRefPtr<MediaQuery>> RustQueryParser::parse_media_query_list(Parser&, Utf16View source)
{
    Vector<NonnullRefPtr<MediaQuery>> queries;
    auto visit = [](void* context, FfiQueryHandle const* handle) {
        auto& queries = *static_cast<Vector<NonnullRefPtr<MediaQuery>>*>(context);
        queries.append(MediaQuery::create(RustQueryHandle::retained(handle)));
    };
    if (!rust_visit_media_query_list(ffi_utf16_view(source), &queries, visit))
        return { MediaQuery::create_not_all() };
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

Optional<RustQueryHandle> RustQueryParser::parse_media_condition(Parser&, Utf16View source)
{
    auto* handle = rust_parse_media_condition(ffi_utf16_view(source));
    if (!handle)
        return {};
    return RustQueryHandle { handle };
}

Optional<RustQueryHandle> RustQueryParser::parse_media_feature(Parser&, Utf16View source)
{
    auto* handle = rust_parse_media_feature(ffi_utf16_view(source));
    if (!handle)
        return {};
    return RustQueryHandle { handle };
}

Optional<RustQueryHandle> Parser::parse_as_supports()
{
    auto context = make_parse_context(ParseContextMode::Syntax);
    auto* handle = rust_parse_supports_condition(ffi_utf16_view(m_source), &context.context);
    if (!handle)
        return {};
    return RustQueryHandle { handle };
}

Optional<RustQueryHandle> Parser::parse_as_supports_declaration()
{
    auto context = make_parse_context(ParseContextMode::Syntax);
    auto* handle = rust_parse_supports_declaration(ffi_utf16_view(m_source), &context.context);
    if (!handle)
        return {};
    return RustQueryHandle { handle };
}

Optional<RustQueryHandle> RustQueryParser::parse_style_query(Parser&, Utf16View source)
{
    auto* handle = rust_parse_style_query(ffi_utf16_view(source));
    if (!handle)
        return {};
    return RustQueryHandle { handle };
}

Optional<Vector<RustQueryParser::ContainerCondition>> RustQueryParser::parse_container_condition_list(Parser&, Utf16View source)
{
    Vector<ContainerCondition> conditions;
    auto visit = [](void* context, u16 const* name, size_t name_length, bool has_name, FfiQueryHandle const* handle) {
        auto& conditions = *static_cast<Vector<ContainerCondition>*>(context);
        Optional<Utf16FlyString> condition_name;
        if (has_name)
            condition_name = Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(name), name_length });
        RefPtr<ContainerQuery> query;
        if (handle)
            query = ContainerQuery::create(RustQueryHandle::retained(handle));
        conditions.append({ .name = move(condition_name), .query = move(query) });
    };
    if (!rust_visit_container_condition_list(ffi_utf16_view(source), &conditions, visit))
        return {};
    return conditions;
}

}
