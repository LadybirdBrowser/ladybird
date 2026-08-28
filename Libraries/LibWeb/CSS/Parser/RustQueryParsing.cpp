/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustQueryParsing.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

using namespace ValueParserFFI;

Vector<NonnullRefPtr<MediaQuery>> RustQueryParser::parse_media_query_list(Utf16View source)
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
    return RustQueryParser::parse_media_query_list(m_source);
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

Optional<RustQueryHandle> Parser::parse_as_supports()
{
    auto context = make_parse_context(ParseContextMode::Syntax);
    auto* handle = rust_parse_supports_condition(ffi_utf16_view(m_source), &context.context);
    if (!handle)
        return {};
    return RustQueryHandle { handle };
}

Optional<RustQueryHandle> RustQueryParser::parse_style_query(Utf16View source)
{
    auto* handle = rust_parse_style_query(ffi_utf16_view(source));
    if (!handle)
        return {};
    return RustQueryHandle { handle };
}

}
