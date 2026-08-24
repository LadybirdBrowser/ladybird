/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/StyleComputeFFI.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

bool MediaFeatureValue::is_ident() const { return m_type == Type::Ident; }
bool MediaFeatureValue::is_integer() const { return m_type == Type::Integer; }
bool MediaFeatureValue::is_length() const { return m_type == Type::Length; }
bool MediaFeatureValue::is_ratio() const { return m_type == Type::Ratio; }
bool MediaFeatureValue::is_resolution() const { return m_type == Type::Resolution; }

Keyword MediaFeatureValue::ident() const
{
    VERIFY(is_ident());
    return m_value->to_keyword();
}

i32 MediaFeatureValue::integer(ComputationContext const& context) const
{
    VERIFY(is_integer());
    return int_from_style_value(m_value->absolutized(context));
}

Length MediaFeatureValue::length(ComputationContext const& context) const
{
    VERIFY(is_length());
    return Length::from_style_value(m_value->absolutized(context), {});
}

Ratio MediaFeatureValue::ratio(ComputationContext const& context) const
{
    VERIFY(is_ratio());
    return m_value->absolutized(context)->as_ratio().resolved();
}

Resolution MediaFeatureValue::resolution(ComputationContext const& context) const
{
    VERIFY(is_resolution());
    return Resolution::from_style_value(m_value->absolutized(context));
}

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
