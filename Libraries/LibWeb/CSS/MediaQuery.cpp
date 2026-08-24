/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/StyleComputeFFI.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

MediaEnvironmentSnapshot::MediaEnvironmentSnapshot(DOM::Document const& document)
{
    if (!document.window())
        return;

    auto computation_context = ComputationContext {
        .length_resolution_context = Length::ResolutionContext::for_document(document),
    };
    m_length_resolution_context = to_ffi_length_resolution_context_with_container_bases(
        computation_context.length_resolution_context, all_container_relative_length_units_mask);

    for (size_t index = 0; index < m_values.size(); ++index) {
        auto queried_value = document.window()->query_media_feature(static_cast<MediaFeatureID>(index));
        if (!queried_value.has_value())
            continue;
        auto& ffi_value = m_values[index];
        auto const& value = queried_value.value();
        if (value.is_ident()) {
            ffi_value.kind = Parser::ValueParserFFI::FfiMediaFeatureValueKind::Ident;
            ffi_value.keyword = to_underlying(value.ident());
        } else if (value.is_integer()) {
            ffi_value.kind = Parser::ValueParserFFI::FfiMediaFeatureValueKind::Integer;
            ffi_value.value = value.integer(computation_context);
        } else if (value.is_length()) {
            ffi_value.kind = Parser::ValueParserFFI::FfiMediaFeatureValueKind::Length;
            ffi_value.value = value.length(computation_context).absolute_length_to_px().to_double();
        } else if (value.is_ratio()) {
            ffi_value.kind = Parser::ValueParserFFI::FfiMediaFeatureValueKind::Ratio;
            auto ratio = value.ratio(computation_context);
            ffi_value.value = ratio.numerator();
            ffi_value.second_value = ratio.denominator();
        } else if (value.is_resolution()) {
            ffi_value.kind = Parser::ValueParserFFI::FfiMediaFeatureValueKind::Resolution;
            ffi_value.value = value.resolution(computation_context).to_dots_per_pixel();
        }
    }
}

NonnullRefPtr<MediaQuery> MediaQuery::create_not_all()
{
    auto media_query = new MediaQuery;
    media_query->m_rust_query_handle = RustQueryHandle { Parser::ValueParserFFI::css_query_create_not_all() };
    media_query->m_negated = true;
    media_query->m_media_type = {
        .name = "all"_utf16_fly_string,
        .known_type = KnownMediaType::All,
    };

    return adopt_ref(*media_query);
}

StringView MediaFeature::serialize_feature_id(MediaFeatureID id)
{
    return string_from_media_feature_id(id);
}

bool MediaFeature::keyword_is_falsey(MediaFeatureID id, Keyword keyword)
{
    return media_feature_keyword_is_falsey(id, keyword);
}

MatchResult MediaFeature::evaluate(BooleanExpressionEvaluationContext const& context) const
{
    (void)context;
    VERIFY_NOT_REACHED();
}

void MediaFeature::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("MediaFeature: {}\n", to_string());
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

void MediaQuery::dump(StringBuilder& builder, int indent_levels) const
{
    dump_indent(builder, indent_levels);
    builder.appendff("Media condition: (matches = {})\n", m_matches);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Negated: {}\n", m_negated);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Type: {}\n", m_media_type.name);

    if (m_media_condition) {
        dump_indent(builder, indent_levels + 1);
        builder.append("Condition:\n"sv);
        m_media_condition->dump(builder, indent_levels + 2);
    }
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

Optional<MediaQuery::KnownMediaType> media_type_from_string(Utf16View name)
{
    if (name.equals_ignoring_ascii_case("all"sv))
        return MediaQuery::KnownMediaType::All;
    if (name.equals_ignoring_ascii_case("print"sv))
        return MediaQuery::KnownMediaType::Print;
    if (name.equals_ignoring_ascii_case("screen"sv))
        return MediaQuery::KnownMediaType::Screen;
    return {};
}

StringView to_string(MediaQuery::KnownMediaType media_type)
{
    switch (media_type) {
    case MediaQuery::KnownMediaType::All:
        return "all"sv;
    case MediaQuery::KnownMediaType::Print:
        return "print"sv;
    case MediaQuery::KnownMediaType::Screen:
        return "screen"sv;
    }
    VERIFY_NOT_REACHED();
}

}
