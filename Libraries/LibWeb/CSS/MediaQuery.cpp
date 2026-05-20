/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

NonnullRefPtr<MediaQuery> MediaQuery::create_not_all()
{
    auto media_query = new MediaQuery;
    media_query->m_negated = true;
    media_query->m_media_type = {
        .name = "all"_fly_string,
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
    auto const& document = context.document;
    VERIFY(document);

    // FIXME: In some cases (e.g. when parsing HTML using DOMParser::parse_from_string()) a document may not be associated with a window -
    //        for now we just return false but perhaps there are some media queries we should still attempt to resolve.
    if (!document->window())
        return MatchResult::False;

    auto queried_value = document->window()->query_media_feature(id());
    if (!queried_value.has_value())
        return MatchResult::False;

    ComputationContext computation_context {
        .length_resolution_context = Length::ResolutionContext::for_document(*document),
    };
    return evaluate_internal(queried_value.value(), computation_context);
}

void MediaFeature::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("MediaFeature: {}\n", to_string());
}

String MediaQuery::to_string() const
{
    StringBuilder builder;

    if (m_negated)
        builder.append("not "sv);

    if (m_negated || m_media_type.known_type != KnownMediaType::All || !m_media_condition) {
        if (m_media_type.known_type.has_value()) {
            builder.append(CSS::to_string(m_media_type.known_type.value()));
        } else {
            builder.append(serialize_an_identifier(m_media_type.name.to_ascii_lowercase()));
        }
        if (m_media_condition)
            builder.append(" and "sv);
    }

    if (m_media_condition) {
        builder.append(m_media_condition->to_string());
    }

    return MUST(builder.to_string());
}

bool MediaQuery::evaluate(DOM::Document const& document)
{
    auto matches_media = [](MediaType const& media) -> MatchResult {
        if (!media.known_type.has_value())
            return MatchResult::False;
        switch (media.known_type.value()) {
        case KnownMediaType::All:
            return MatchResult::True;
        case KnownMediaType::Print:
            // FIXME: Enable for printing, when we have printing!
            return MatchResult::False;
        case KnownMediaType::Screen:
            // FIXME: Disable for printing, when we have printing!
            return MatchResult::True;
        }
        VERIFY_NOT_REACHED();
    };

    MatchResult result = matches_media(m_media_type);

    if ((result != MatchResult::False) && m_media_condition)
        result = result && m_media_condition->evaluate({ .document = document });

    if (m_negated)
        result = negate(result);

    m_matches = result == MatchResult::True;
    return m_matches;
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
String serialize_a_media_query_list(Vector<NonnullRefPtr<MediaQuery>> const& media_queries)
{
    // 1. If the media query list is empty, then return the empty string.
    if (media_queries.is_empty())
        return String {};

    // 2. Serialize each media query in the list of media queries, in the same order as they
    // appear in the media query list, and then serialize the list.
    return MUST(String::join(", "sv, media_queries));
}

Optional<MediaQuery::KnownMediaType> media_type_from_string(StringView name)
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
