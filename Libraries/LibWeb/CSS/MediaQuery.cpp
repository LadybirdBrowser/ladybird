/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/StyleComputeFFI.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

MediaEnvironmentSnapshot::MediaEnvironmentSnapshot(DOM::Document const& document)
{
    if (!document.window())
        return;

    m_length_resolution_context = to_ffi_length_resolution_context_with_container_bases(
        Length::ResolutionContext::for_document(document), all_container_relative_length_units_mask);

    for (size_t index = 0; index < m_values.size(); ++index)
        m_values[index] = document.window()->query_media_feature(static_cast<MediaFeatureID>(index));
}

NonnullRefPtr<MediaQuery> MediaQuery::create_not_all()
{
    return create(RustQueryHandle { Parser::ValueParserFFI::css_query_create_not_all() });
}

Utf16String MediaQuery::to_string() const
{
    Utf16StringBuilder builder;
    serialize_to(builder);
    return builder.to_string();
}

void MediaQuery::serialize_to(Utf16StringBuilder& builder) const
{
    auto append_serialized_query = [](void* context, u16 const* code_units, size_t length) {
        static_cast<Utf16StringBuilder*>(context)->append({ reinterpret_cast<char16_t const*>(code_units), length });
    };
    VERIFY(Parser::ValueParserFFI::css_query_serialize_media_query(m_rust_query_handle.data(), &builder, append_serialized_query));
}

bool MediaQuery::evaluate(MediaEnvironmentSnapshot const& environment)
{
    m_matches = Parser::ValueParserFFI::css_query_evaluate_media(m_rust_query_handle.data(), environment.ffi_environment());
    return m_matches;
}

bool MediaQuery::evaluate(DOM::Document const& document)
{
    return evaluate(MediaEnvironmentSnapshot { document });
}

MatchResult evaluate_media_condition(RustQueryHandle const& handle, MediaEnvironmentSnapshot const& environment)
{
    auto result = Parser::ValueParserFFI::css_query_evaluate_media_condition(handle.data(), environment.ffi_environment());
    VERIFY(result <= to_underlying(MatchResult::Unknown));
    return static_cast<MatchResult>(result);
}

void MediaQuery::dump(StringBuilder& builder, int indent_levels) const
{
    dump_indent(builder, indent_levels);
    builder.appendff("Media query: `{}` (matches = {})\n", to_string(), m_matches);
}

// https://www.w3.org/TR/cssom-1/#serialize-a-media-query-list
Utf16String serialize_a_media_query_list(Vector<NonnullRefPtr<MediaQuery>> const& media_queries)
{
    // 1. If the media query list is empty, then return the empty string.
    if (media_queries.is_empty())
        return {};

    // 2. Serialize each media query in the list of media queries, in the same order as they
    // appear in the media query list, and then serialize the list.
    Utf16StringBuilder builder;
    bool first = true;
    for (auto const& media_query : media_queries) {
        if (first)
            first = false;
        else
            builder.append_ascii(", "sv);
        media_query->serialize_to(builder);
    }
    return builder.to_string();
}

}
