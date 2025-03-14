/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>

namespace Web::CSS {

NonnullRefPtr<MediaQuery> MediaQuery::create_not_all()
{
    auto media_query = new MediaQuery;
    media_query->m_negated = true;
    media_query->m_media_type = MediaType::All;

    return adopt_ref(*media_query);
}

String MediaFeatureValue::to_string() const
{
    return m_value.visit(
        [](Keyword const& ident) { return MUST(String::from_utf8(string_from_keyword(ident))); },
        [](LengthOrCalculated const& length) { return length.to_string(); },
        [](Ratio const& ratio) { return ratio.to_string(); },
        [](ResolutionOrCalculated const& resolution) { return resolution.to_string(); },
        [](IntegerOrCalculated const& integer) {
            if (integer.is_calculated())
                return integer.calculated()->to_string(CSSStyleValue::SerializationMode::Normal);
            return String::number(integer.value());
        });
}

bool MediaFeatureValue::is_same_type(MediaFeatureValue const& other) const
{
    return m_value.visit(
        [&](Keyword const&) { return other.is_ident(); },
        [&](LengthOrCalculated const&) { return other.is_length(); },
        [&](Ratio const&) { return other.is_ratio(); },
        [&](ResolutionOrCalculated const&) { return other.is_resolution(); },
        [&](IntegerOrCalculated const&) { return other.is_integer(); });
}

String MediaFeature::to_string() const
{
    auto comparison_string = [](Comparison comparison) -> StringView {
        switch (comparison) {
        case Comparison::Equal:
            return "="sv;
        case Comparison::LessThan:
            return "<"sv;
        case Comparison::LessThanOrEqual:
            return "<="sv;
        case Comparison::GreaterThan:
            return ">"sv;
        case Comparison::GreaterThanOrEqual:
            return ">="sv;
        }
        VERIFY_NOT_REACHED();
    };

    switch (m_type) {
    case Type::IsTrue:
        return MUST(String::from_utf8(string_from_media_feature_id(m_id)));
    case Type::ExactValue:
        return MUST(String::formatted("{}: {}", string_from_media_feature_id(m_id), value().to_string()));
    case Type::MinValue:
        return MUST(String::formatted("min-{}: {}", string_from_media_feature_id(m_id), value().to_string()));
    case Type::MaxValue:
        return MUST(String::formatted("max-{}: {}", string_from_media_feature_id(m_id), value().to_string()));
    case Type::Range: {
        auto& range = this->range();
        if (!range.right_comparison.has_value())
            return MUST(String::formatted("{} {} {}", range.left_value.to_string(), comparison_string(range.left_comparison), string_from_media_feature_id(m_id)));

        return MUST(String::formatted("{} {} {} {} {}", range.left_value.to_string(), comparison_string(range.left_comparison), string_from_media_feature_id(m_id), comparison_string(*range.right_comparison), range.right_value->to_string()));
    }
    }

    VERIFY_NOT_REACHED();
}

MatchResult MediaFeature::evaluate(HTML::Window const* window) const
{
    VERIFY(window);
    auto maybe_queried_value = window->query_media_feature(m_id);
    if (!maybe_queried_value.has_value())
        return MatchResult::False;
    auto queried_value = maybe_queried_value.release_value();

    CalculationResolutionContext calculation_context {
        .length_resolution_context = Length::ResolutionContext::for_window(*window),
    };
    switch (m_type) {
    case Type::IsTrue:
        if (queried_value.is_integer())
            return as_match_result(queried_value.integer().resolved(calculation_context) != 0);
        if (queried_value.is_length()) {
            auto length = queried_value.length().resolved(calculation_context);
            return as_match_result(length->raw_value() != 0);
        }
        // FIXME: I couldn't figure out from the spec how ratios should be evaluated in a boolean context.
        if (queried_value.is_ratio())
            return as_match_result(!queried_value.ratio().is_degenerate());
        if (queried_value.is_resolution())
            return as_match_result(queried_value.resolution().resolved(calculation_context).map([](auto& it) { return it.to_dots_per_pixel(); }).value_or(0) != 0);
        if (queried_value.is_ident()) {
            // NOTE: It is not technically correct to always treat `no-preference` as false, but every
            //       media-feature that accepts it as a value treats it as false, so good enough. :^)
            //       If other features gain this property for other keywords in the future, we can
            //       add more robust handling for them then.
            return as_match_result(queried_value.ident() != Keyword::None
                && queried_value.ident() != Keyword::NoPreference);
        }
        return MatchResult::False;

    case Type::ExactValue:
        return as_match_result(compare(*window, value(), Comparison::Equal, queried_value));

    case Type::MinValue:
        return as_match_result(compare(*window, queried_value, Comparison::GreaterThanOrEqual, value()));

    case Type::MaxValue:
        return as_match_result(compare(*window, queried_value, Comparison::LessThanOrEqual, value()));

    case Type::Range: {
        auto& range = this->range();
        if (!compare(*window, range.left_value, range.left_comparison, queried_value))
            return MatchResult::False;

        if (range.right_comparison.has_value())
            if (!compare(*window, queried_value, *range.right_comparison, *range.right_value))
                return MatchResult::False;

        return MatchResult::True;
    }
    }

    VERIFY_NOT_REACHED();
}

bool MediaFeature::compare(HTML::Window const& window, MediaFeatureValue left, Comparison comparison, MediaFeatureValue right)
{
    if (!left.is_same_type(right))
        return false;

    if (left.is_ident()) {
        if (comparison == Comparison::Equal)
            return left.ident() == right.ident();
        return false;
    }

    CalculationResolutionContext calculation_context {
        .length_resolution_context = Length::ResolutionContext::for_window(window),
    };

    if (left.is_integer()) {
        switch (comparison) {
        case Comparison::Equal:
            return left.integer().resolved(calculation_context).value_or(0) == right.integer().resolved(calculation_context).value_or(0);
        case Comparison::LessThan:
            return left.integer().resolved(calculation_context).value_or(0) < right.integer().resolved(calculation_context).value_or(0);
        case Comparison::LessThanOrEqual:
            return left.integer().resolved(calculation_context).value_or(0) <= right.integer().resolved(calculation_context).value_or(0);
        case Comparison::GreaterThan:
            return left.integer().resolved(calculation_context).value_or(0) > right.integer().resolved(calculation_context).value_or(0);
        case Comparison::GreaterThanOrEqual:
            return left.integer().resolved(calculation_context).value_or(0) >= right.integer().resolved(calculation_context).value_or(0);
        }
        VERIFY_NOT_REACHED();
    }

    if (left.is_length()) {
        CSSPixels left_px;
        CSSPixels right_px;
        auto left_length = left.length().resolved(calculation_context).value_or(Length::make_px(0));
        auto right_length = right.length().resolved(calculation_context).value_or(Length::make_px(0));
        // Save ourselves some work if neither side is a relative length.
        if (left_length.is_absolute() && right_length.is_absolute()) {
            left_px = left_length.absolute_length_to_px();
            right_px = right_length.absolute_length_to_px();
        } else {
            auto viewport_rect = window.page().web_exposed_screen_area();

            auto const& initial_font = window.associated_document().style_computer().initial_font();
            Gfx::FontPixelMetrics const& initial_font_metrics = initial_font.pixel_metrics();
            Length::FontMetrics font_metrics { CSSPixels { initial_font.point_size() }, initial_font_metrics };

            left_px = left_length.to_px(viewport_rect, font_metrics, font_metrics);
            right_px = right_length.to_px(viewport_rect, font_metrics, font_metrics);
        }

        switch (comparison) {
        case Comparison::Equal:
            return left_px == right_px;
        case Comparison::LessThan:
            return left_px < right_px;
        case Comparison::LessThanOrEqual:
            return left_px <= right_px;
        case Comparison::GreaterThan:
            return left_px > right_px;
        case Comparison::GreaterThanOrEqual:
            return left_px >= right_px;
        }

        VERIFY_NOT_REACHED();
    }

    if (left.is_ratio()) {
        auto left_decimal = left.ratio().value();
        auto right_decimal = right.ratio().value();

        switch (comparison) {
        case Comparison::Equal:
            return left_decimal == right_decimal;
        case Comparison::LessThan:
            return left_decimal < right_decimal;
        case Comparison::LessThanOrEqual:
            return left_decimal <= right_decimal;
        case Comparison::GreaterThan:
            return left_decimal > right_decimal;
        case Comparison::GreaterThanOrEqual:
            return left_decimal >= right_decimal;
        }
        VERIFY_NOT_REACHED();
    }

    if (left.is_resolution()) {
        auto left_dppx = left.resolution().resolved(calculation_context).map([](auto& it) { return it.to_dots_per_pixel(); }).value_or(0);
        auto right_dppx = right.resolution().resolved(calculation_context).map([](auto& it) { return it.to_dots_per_pixel(); }).value_or(0);

        switch (comparison) {
        case Comparison::Equal:
            return left_dppx == right_dppx;
        case Comparison::LessThan:
            return left_dppx < right_dppx;
        case Comparison::LessThanOrEqual:
            return left_dppx <= right_dppx;
        case Comparison::GreaterThan:
            return left_dppx > right_dppx;
        case Comparison::GreaterThanOrEqual:
            return left_dppx >= right_dppx;
        }
        VERIFY_NOT_REACHED();
    }

    VERIFY_NOT_REACHED();
}

void MediaFeature::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("MediaFeature: {}", to_string());
}

String MediaQuery::to_string() const
{
    StringBuilder builder;

    if (m_negated)
        builder.append("not "sv);

    if (m_negated || m_media_type != MediaType::All || !m_media_condition) {
        builder.append(CSS::to_string(m_media_type));
        if (m_media_condition)
            builder.append(" and "sv);
    }

    if (m_media_condition) {
        builder.append(m_media_condition->to_string());
    }

    return MUST(builder.to_string());
}

bool MediaQuery::evaluate(HTML::Window const& window)
{
    auto matches_media = [](MediaType media) -> MatchResult {
        switch (media) {
        case MediaType::All:
            return MatchResult::True;
        case MediaType::Print:
            // FIXME: Enable for printing, when we have printing!
            return MatchResult::False;
        case MediaType::Screen:
            // FIXME: Disable for printing, when we have printing!
            return MatchResult::True;
        case MediaType::Unknown:
            return MatchResult::False;
        // Deprecated, must never match:
        case MediaType::TTY:
        case MediaType::TV:
        case MediaType::Projection:
        case MediaType::Handheld:
        case MediaType::Braille:
        case MediaType::Embossed:
        case MediaType::Aural:
        case MediaType::Speech:
            return MatchResult::False;
        }
        VERIFY_NOT_REACHED();
    };

    MatchResult result = matches_media(m_media_type);

    if ((result == MatchResult::True) && m_media_condition)
        result = m_media_condition->evaluate(&window);

    if (m_negated)
        result = negate(result);

    m_matches = result == MatchResult::True;
    return m_matches;
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

MediaQuery::MediaType media_type_from_string(StringView name)
{
    if (name.equals_ignoring_ascii_case("all"sv))
        return MediaQuery::MediaType::All;
    if (name.equals_ignoring_ascii_case("aural"sv))
        return MediaQuery::MediaType::Aural;
    if (name.equals_ignoring_ascii_case("braille"sv))
        return MediaQuery::MediaType::Braille;
    if (name.equals_ignoring_ascii_case("embossed"sv))
        return MediaQuery::MediaType::Embossed;
    if (name.equals_ignoring_ascii_case("handheld"sv))
        return MediaQuery::MediaType::Handheld;
    if (name.equals_ignoring_ascii_case("print"sv))
        return MediaQuery::MediaType::Print;
    if (name.equals_ignoring_ascii_case("projection"sv))
        return MediaQuery::MediaType::Projection;
    if (name.equals_ignoring_ascii_case("screen"sv))
        return MediaQuery::MediaType::Screen;
    if (name.equals_ignoring_ascii_case("speech"sv))
        return MediaQuery::MediaType::Speech;
    if (name.equals_ignoring_ascii_case("tty"sv))
        return MediaQuery::MediaType::TTY;
    if (name.equals_ignoring_ascii_case("tv"sv))
        return MediaQuery::MediaType::TV;
    return MediaQuery::MediaType::Unknown;
}

StringView to_string(MediaQuery::MediaType media_type)
{
    switch (media_type) {
    case MediaQuery::MediaType::All:
        return "all"sv;
    case MediaQuery::MediaType::Aural:
        return "aural"sv;
    case MediaQuery::MediaType::Braille:
        return "braille"sv;
    case MediaQuery::MediaType::Embossed:
        return "embossed"sv;
    case MediaQuery::MediaType::Handheld:
        return "handheld"sv;
    case MediaQuery::MediaType::Print:
        return "print"sv;
    case MediaQuery::MediaType::Projection:
        return "projection"sv;
    case MediaQuery::MediaType::Screen:
        return "screen"sv;
    case MediaQuery::MediaType::Speech:
        return "speech"sv;
    case MediaQuery::MediaType::TTY:
        return "tty"sv;
    case MediaQuery::MediaType::TV:
        return "tv"sv;
    case MediaQuery::MediaType::Unknown:
        return "unknown"sv;
    }
    VERIFY_NOT_REACHED();
}

}
