/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2026, Ruben Kelevra <rubenkelevra@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Find.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <AK/Utf8View.h>
#include <LibCore/Directory.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#ifndef NDEBUG
#    include <LibCore/ThreadEventQueue.h>
#endif
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibTextCodec/Decoder.h>
#include <LibThreading/BackgroundAction.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/IDNA.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/URL.h>

namespace WebView {

static constexpr auto file_url_prefix = "file://"sv;
static constexpr auto local_index_file_name = "AutocompleteIndex.json"sv;
static constexpr mode_t local_index_directory_permissions = 0700;
static constexpr mode_t local_index_file_permissions = 0600;
static constexpr size_t local_index_loader_absolute_entry_cap = 500'000;
static constexpr size_t max_index_prefix_length = 48;
static constexpr u32 max_typo_distance = 2;
static constexpr int source_removal_rebuild_delay_ms = 60'000;
static constexpr i64 shutdown_flush_wait_timeout_ms = 10'000;
static constexpr auto local_index_rebuild_placeholder = "Rebuilding local suggestion index..."sv;
static constexpr auto local_index_search_title_data_key = "searchTitleDataIndexed"sv;

static Autocomplete* s_first_live_autocomplete_instance { nullptr };

static constexpr auto builtin_autocomplete_engines = to_array<AutocompleteEngine>({
    { "DuckDuckGo"sv, "https://duckduckgo.com/ac/?q={}"sv },
    { "Google"sv, "https://www.google.com/complete/search?client=chrome&q={}"sv },
    { "Yahoo"sv, "https://search.yahoo.com/sugg/gossip/gossip-us-ura/?output=sd1&command={}"sv },
});

struct LocalSuggestionEntry {
    String text;
    Optional<String> title;
    String normalized_text;
    SuggestionKind kind { SuggestionKind::QueryCompletion };
    SuggestionSource source { SuggestionSource::History };
    double frequency { 0.0 };
    i64 last_used_unix_seconds { 0 };
};

struct LoadedLocalSuggestionIndex {
    Vector<LocalSuggestionEntry> entries;
    bool search_title_data_indexed { false };
};

struct SearchResultNavigationFilter {
    Optional<String> search_engine_host;
    Optional<String> autocomplete_engine_host;
};

static String normalize_suggestion_text(StringView text)
{
    auto normalized = MUST(String::from_utf8(text.trim_whitespace()));
    return normalized.to_ascii_lowercase();
}

static Optional<String> normalize_title_for_storage(Optional<StringView> title)
{
    if (!title.has_value())
        return {};

    auto trimmed_title = title->trim_whitespace();
    if (trimmed_title.is_empty())
        return {};

    return MUST(String::from_utf8(trimmed_title));
}

static Optional<size_t> common_www_prefix_length(StringView text)
{
    if (text.starts_with("www."sv))
        return 4;

    if (text.length() >= 5
        && text.starts_with("www"sv)
        && is_ascii_digit(text[3])
        && text[4] == '.')
        return 5;

    return {};
}

static bool is_bare_common_www_prefix(StringView text)
{
    return text == "www"sv
        || (text.length() == 4
            && text.starts_with("www"sv)
            && is_ascii_digit(text[3]));
}

static StringView text_without_common_www_prefix(StringView text)
{
    if (auto prefix_length = common_www_prefix_length(text); prefix_length.has_value())
        return text.substring_view(*prefix_length);
    return text;
}

static Optional<String> normalize_host_for_matching(URL::Host const& host)
{
    auto serialized_host = host.serialize();
    auto host_without_www = text_without_common_www_prefix(serialized_host.bytes_as_string_view());
    if (host_without_www.is_empty())
        return {};
    return normalize_suggestion_text(host_without_www);
}

static Optional<URL::URL> parse_query_url_template_for_host(StringView query_url_template)
{
    if (auto parsed_url = URL::Parser::basic_parse(query_url_template); parsed_url.has_value())
        return parsed_url;

    auto query_url = MUST(String::from_utf8(query_url_template));
    query_url = MUST(query_url.replace("%s"sv, "query"sv, ReplaceMode::All));
    query_url = MUST(query_url.replace("{}"sv, "query"sv, ReplaceMode::All));
    return URL::Parser::basic_parse(query_url.bytes_as_string_view());
}

static SearchResultNavigationFilter search_result_navigation_filter_from_settings()
{
    SearchResultNavigationFilter filter;

    if (auto const& search_engine = Application::settings().search_engine(); search_engine.has_value()) {
        if (auto parsed_search_url = parse_query_url_template_for_host(search_engine->query_url.bytes_as_string_view()); parsed_search_url.has_value() && parsed_search_url->host().has_value())
            filter.search_engine_host = normalize_host_for_matching(parsed_search_url->host().value());
    }

    if (auto const& autocomplete_engine = Application::settings().autocomplete_engine(); autocomplete_engine.has_value()) {
        if (auto parsed_autocomplete_url = parse_query_url_template_for_host(autocomplete_engine->query_url); parsed_autocomplete_url.has_value() && parsed_autocomplete_url->host().has_value())
            filter.autocomplete_engine_host = normalize_host_for_matching(parsed_autocomplete_url->host().value());
    }

    return filter;
}

static bool should_skip_search_result_navigation(StringView navigated_text, SearchResultNavigationFilter const& filter)
{
    if (!filter.search_engine_host.has_value() && !filter.autocomplete_engine_host.has_value())
        return false;

    auto parsed_url = URL::Parser::basic_parse(navigated_text);
    if (!parsed_url.has_value())
        parsed_url = sanitize_url(navigated_text, {});
    if (!parsed_url.has_value())
        return false;

    if (!parsed_url->query().has_value() || !parsed_url->host().has_value())
        return false;

    auto normalized_navigated_host = normalize_host_for_matching(parsed_url->host().value());
    if (!normalized_navigated_host.has_value())
        return false;

    if (filter.search_engine_host.has_value() && *normalized_navigated_host == *filter.search_engine_host)
        return true;
    if (filter.autocomplete_engine_host.has_value() && *normalized_navigated_host == *filter.autocomplete_engine_host)
        return true;

    return false;
}

static bool is_title_keyword_separator_ascii(u8 byte)
{
    return byte == '.'
        || byte == ':'
        || byte == ','
        || byte == '/'
        || byte == '\\'
        || byte == '-';
}

static bool is_utf8_em_dash_at(StringView text, size_t index)
{
    auto bytes = text.bytes();
    if (index + 2 >= bytes.size())
        return false;

    return bytes[index] == 0xe2
        && bytes[index + 1] == 0x80
        && bytes[index + 2] == 0x94;
}

static StringView trim_title_keyword_boundary_separators(StringView segment)
{
    size_t start = 0;
    size_t end = segment.length();

    while (start < end) {
        if (is_title_keyword_separator_ascii(static_cast<u8>(segment[start]))) {
            ++start;
            continue;
        }

        if (is_utf8_em_dash_at(segment, start)) {
            start += 3;
            continue;
        }

        break;
    }

    while (start < end) {
        if (is_title_keyword_separator_ascii(static_cast<u8>(segment[end - 1]))) {
            --end;
            continue;
        }

        if (end >= 3 && end - 3 >= start && is_utf8_em_dash_at(segment, end - 3)) {
            end -= 3;
            continue;
        }

        break;
    }

    if (start >= end)
        return {};
    return segment.substring_view(start, end - start);
}

static Vector<String> split_title_keyword_parts(StringView segment)
{
    Vector<String> parts;
    if (segment.is_empty())
        return parts;

    size_t part_start = 0;
    size_t index = 0;

    while (index < segment.length()) {
        size_t separator_length = 0;
        if (is_title_keyword_separator_ascii(static_cast<u8>(segment[index])))
            separator_length = 1;
        else if (is_utf8_em_dash_at(segment, index))
            separator_length = 3;

        if (separator_length == 0) {
            ++index;
            continue;
        }

        if (index > part_start)
            parts.append(MUST(String::from_utf8(segment.substring_view(part_start, index - part_start))));

        index += separator_length;
        part_start = index;
    }

    if (part_start < segment.length())
        parts.append(MUST(String::from_utf8(segment.substring_view(part_start))));

    return parts;
}

static Vector<String> title_keywords_for_indexing(StringView normalized_title)
{
    Vector<String> keywords;
    HashTable<String> seen_keywords;

    auto add_keyword = [&](StringView keyword) {
        auto trimmed_keyword = keyword.trim_whitespace();
        if (trimmed_keyword.is_empty())
            return;

        auto keyword_string = MUST(String::from_utf8(trimmed_keyword));
        if (seen_keywords.contains(keyword_string))
            return;

        seen_keywords.set(keyword_string);
        keywords.append(move(keyword_string));
    };

    size_t segment_start = 0;
    auto flush_segment = [&](size_t segment_end) {
        if (segment_end <= segment_start)
            return;

        auto segment = normalized_title.substring_view(segment_start, segment_end - segment_start);
        add_keyword(segment);

        auto boundary_trimmed_segment = trim_title_keyword_boundary_separators(segment);
        if (!boundary_trimmed_segment.is_empty() && boundary_trimmed_segment != segment)
            add_keyword(boundary_trimmed_segment);

        for (auto const& part : split_title_keyword_parts(segment))
            add_keyword(part.bytes_as_string_view());
    };

    auto utf8_title = Utf8View { normalized_title };
    if (!utf8_title.validate()) {
        for (size_t index = 0; index < normalized_title.length(); ++index) {
            if (!is_ascii_space(static_cast<u8>(normalized_title[index])))
                continue;

            flush_segment(index);
            segment_start = index + 1;
        }
    } else {
        for (auto it = utf8_title.begin(); it != utf8_title.end(); ++it) {
            if (!Unicode::code_point_has_white_space_property(*it))
                continue;

            auto index = utf8_title.byte_offset_of(it);
            flush_segment(index);
            segment_start = index + it.underlying_code_point_length_in_bytes();
        }
    }

    flush_segment(normalized_title.length());
    return keywords;
}

static bool contains_whitespace(StringView text)
{
    auto utf8_view = Utf8View { text };
    if (!utf8_view.validate())
        return any_of(text.bytes(), [](auto byte) { return is_ascii_space(byte); });

    for (auto code_point : utf8_view) {
        if (Unicode::code_point_has_white_space_property(code_point))
            return true;
    }

    return false;
}

static bool looks_like_navigational(StringView query)
{
    auto trimmed_query = query.trim_whitespace();
    if (contains_whitespace(trimmed_query))
        return false;

    return trimmed_query.starts_with("http://"sv)
        || trimmed_query.starts_with("https://"sv)
        || is_bare_common_www_prefix(trimmed_query)
        || trimmed_query.starts_with("www."sv)
        || (trimmed_query.length() >= 5
            && trimmed_query.starts_with("www"sv)
            && is_ascii_digit(trimmed_query[3])
            && trimmed_query[4] == '.')
        || trimmed_query.starts_with("localhost"sv)
        || trimmed_query.contains('/')
        || trimmed_query.contains('.');
}

static bool is_disallowed_local_suggestion_scheme(StringView scheme)
{
    return scheme.equals_ignoring_ascii_case("data"sv)
        || scheme.equals_ignoring_ascii_case("javascript"sv)
        || scheme.equals_ignoring_ascii_case("vbscript"sv)
        || scheme.equals_ignoring_ascii_case("blob"sv);
}

static String decode_percent_encoded_component_for_display(StringView component)
{
    auto decoded_component = URL::percent_decode(component);
    auto decoded_component_view = decoded_component.view();

    if (!Utf8View(decoded_component_view).validate(AllowLonelySurrogates::No))
        return MUST(String::from_utf8(component));

    return MUST(String::from_utf8(decoded_component_view));
}

static String sanitize_navigational_text_for_storage(StringView text)
{
    auto trimmed_text = text.trim_whitespace();
    if (trimmed_text.is_empty())
        return {};

    auto parsed_url = URL::Parser::basic_parse(trimmed_text);
    if (!parsed_url.has_value())
        parsed_url = sanitize_url(trimmed_text, {});

    if (!parsed_url.has_value()) {
        auto redacted_text = trimmed_text;
        if (auto fragment_start = redacted_text.find('#'); fragment_start.has_value())
            redacted_text = redacted_text.substring_view(0, *fragment_start);
        if (auto query_start = redacted_text.find('?'); query_start.has_value())
            redacted_text = redacted_text.substring_view(0, *query_start);
        if (redacted_text.starts_with("https://"sv))
            redacted_text = redacted_text.substring_view(8);
        redacted_text = text_without_common_www_prefix(redacted_text);
        while (redacted_text.length() > 1 && redacted_text.ends_with('/'))
            redacted_text = redacted_text.substring_view(0, redacted_text.length() - 1);
        return MUST(String::from_utf8(redacted_text.trim_whitespace()));
    }

    auto url = parsed_url.release_value();
    if (is_disallowed_local_suggestion_scheme(url.scheme()))
        return {};

    if (!url.host().has_value())
        return {};

    url.set_username(""sv);
    url.set_password(""sv);
    url.set_query({});
    url.set_fragment({});

    auto normalized_path = [&]() -> String {
        auto path = url.serialize_path();
        auto path_view = path.bytes_as_string_view();
        while (path_view.length() > 1 && path_view.ends_with('/'))
            path_view = path_view.substring_view(0, path_view.length() - 1);
        if (path_view == "/"sv)
            return {};
        return decode_percent_encoded_component_for_display(path_view);
    }();

    auto append_host_for_storage = [&](StringBuilder& builder) {
        auto const& host = url.host().value();

        if (url.scheme() != "https"sv)
            builder.appendff("{}://", url.scheme());

        if (host.is_domain()) {
            auto serialized_host = host.serialize();
            auto unicode_host = Unicode::IDNA::to_unicode(Utf8View(serialized_host));
            if (unicode_host.is_error())
                builder.append(text_without_common_www_prefix(serialized_host.bytes_as_string_view()));
            else {
                auto unicode_host_text = unicode_host.release_value();
                builder.append(text_without_common_www_prefix(unicode_host_text.bytes_as_string_view()));
            }
        } else {
            builder.append(host.serialize());
        }

        if (url.port().has_value())
            builder.appendff(":{}", *url.port());
    };

    StringBuilder builder;
    append_host_for_storage(builder);

    builder.append(normalized_path);

    return builder.to_string_without_validation();
}

static Optional<String> sanitize_navigational_host_only_for_storage(StringView text)
{
    auto trimmed_text = text.trim_whitespace();
    if (trimmed_text.is_empty())
        return {};

    auto parsed_url = URL::Parser::basic_parse(trimmed_text);
    if (!parsed_url.has_value())
        parsed_url = sanitize_url(trimmed_text, {});
    if (!parsed_url.has_value())
        return {};

    auto url = parsed_url.release_value();
    if (is_disallowed_local_suggestion_scheme(url.scheme()))
        return {};

    if (!url.host().has_value())
        return {};

    url.set_username(""sv);
    url.set_password(""sv);
    url.set_query({});
    url.set_fragment({});

    auto path = url.serialize_path();
    auto path_view = path.bytes_as_string_view();
    while (path_view.length() > 1 && path_view.ends_with('/'))
        path_view = path_view.substring_view(0, path_view.length() - 1);
    if (path_view.is_empty() || path_view == "/"sv)
        return {};

    StringBuilder builder;
    if (url.scheme() != "https"sv)
        builder.appendff("{}://", url.scheme());

    auto const& host = url.host().value();
    if (host.is_domain()) {
        auto serialized_host = host.serialize();
        auto unicode_host = Unicode::IDNA::to_unicode(Utf8View(serialized_host));
        if (unicode_host.is_error())
            builder.append(text_without_common_www_prefix(serialized_host.bytes_as_string_view()));
        else {
            auto unicode_host_text = unicode_host.release_value();
            builder.append(text_without_common_www_prefix(unicode_host_text.bytes_as_string_view()));
        }
    } else {
        builder.append(host.serialize());
    }

    if (url.port().has_value())
        builder.appendff(":{}", *url.port());

    return builder.to_string_without_validation();
}

static bool can_store_title_for_navigational_text(StringView text)
{
    auto trimmed_text = text.trim_whitespace();
    if (trimmed_text.is_empty())
        return false;

    auto parsed_url = URL::Parser::basic_parse(trimmed_text);
    if (parsed_url.has_value()) {
        auto const& url = parsed_url.value();

        if (is_disallowed_local_suggestion_scheme(url.scheme()))
            return false;

        if (!url.host().has_value())
            return false;

        if (url.query().has_value() || url.fragment().has_value())
            return false;

        if (!url.username().is_empty() || !url.password().is_empty())
            return false;

        return true;
    }

    // Fallback path for navigation-like inputs that do not fully parse as URLs.
    return !trimmed_text.contains('?')
        && !trimmed_text.contains('#');
}

static bool should_exclude_from_local_index(StringView text)
{
    return text.equals_ignoring_ascii_case("about:"sv)
        || text.starts_with("about://"sv)
        || text.equals_ignoring_ascii_case("about:newtab"sv)
        || text.equals_ignoring_ascii_case("about:blank"sv)
        || text.starts_with("data:"sv)
        || text.starts_with("javascript:"sv)
        || text.starts_with("vbscript:"sv)
        || text.starts_with("blob:"sv);
}

static String dedup_key_for_suggestion_text(StringView suggestion_text)
{
    auto normalized_text = normalize_suggestion_text(suggestion_text);
    if (normalized_text.is_empty())
        return {};

    auto normalized_view = normalized_text.bytes_as_string_view();
    if (!looks_like_navigational(normalized_view))
        return normalized_text;

    auto sanitized_text = sanitize_navigational_text_for_storage(suggestion_text);
    if (sanitized_text.is_empty())
        return normalized_text;

    auto dedup_view = sanitized_text.bytes_as_string_view();
    if (dedup_view.starts_with("http://"sv))
        dedup_view = dedup_view.substring_view(7);
    else if (dedup_view.starts_with("https://"sv))
        dedup_view = dedup_view.substring_view(8);

    return normalize_suggestion_text(dedup_view);
}

static StringView text_without_url_scheme_for_matching(StringView text)
{
    if (text.starts_with("http://"sv))
        return text.substring_view(7);
    if (text.starts_with("https://"sv))
        return text.substring_view(8);
    return text;
}

static String normalize_remote_suggestion_for_display(StringView suggestion_text)
{
    auto trimmed_text = suggestion_text.trim_whitespace();
    if (trimmed_text.is_empty())
        return {};

    if (!looks_like_navigational(trimmed_text))
        return MUST(String::from_utf8(trimmed_text));

    auto sanitized_text = sanitize_navigational_text_for_storage(trimmed_text);
    if (sanitized_text.is_empty())
        return {};

    auto display_text = sanitized_text.bytes_as_string_view();
    if (display_text.starts_with("http://"sv))
        display_text = display_text.substring_view(7);

    return MUST(String::from_utf8(display_text));
}

struct NormalizedQuery {
    String text;
    bool show_top_navigational_results { false };
};

static NormalizedQuery normalize_query_for_matching(StringView query, bool prefer_navigational)
{
    auto normalized_query = normalize_suggestion_text(query);
    if (!prefer_navigational)
        return { move(normalized_query), false };

    auto query_view = normalized_query.bytes_as_string_view();
    if (query_view.starts_with("http://"sv))
        query_view = query_view.substring_view(7);
    else if (query_view.starts_with("https://"sv))
        query_view = query_view.substring_view(8);

    auto show_top_navigational_results = false;
    if (is_bare_common_www_prefix(query_view)) {
        show_top_navigational_results = true;
        query_view = {};
    } else {
        query_view = text_without_common_www_prefix(query_view);
        if (query_view.is_empty())
            show_top_navigational_results = true;
    }

    if (query_view.is_empty())
        return { {}, show_top_navigational_results };

    return { MUST(String::from_utf8(query_view)), show_top_navigational_results };
}

static Optional<StringView> text_without_common_www_prefix_for_matching(StringView normalized_text)
{
    auto host_text = text_without_url_scheme_for_matching(normalized_text);
    auto without_www = text_without_common_www_prefix(host_text);
    if (without_www == host_text)
        return {};
    return without_www;
}

static Vector<String> tokenize(StringView normalized_text)
{
    Vector<String> tokens;
    StringBuilder token_builder;

    auto flush_token = [&]() {
        if (token_builder.is_empty())
            return;
        tokens.append(token_builder.to_string_without_validation());
        token_builder.clear();
    };

    for (auto byte : normalized_text.bytes()) {
        if (is_ascii_alphanumeric(byte))
            token_builder.append(byte);
        else
            flush_token();
    }

    flush_token();
    return tokens;
}

template<typename LeftAt, typename RightAt>
static u32 bounded_edit_distance_impl(size_t left_length, size_t right_length, u32 max_distance, LeftAt left_at, RightAt right_at)
{
    auto const length_delta = left_length > right_length ? left_length - right_length : right_length - left_length;
    if (length_delta > max_distance)
        return max_distance + 1;

    Vector<u32> previous_row;
    previous_row.resize(right_length + 1);

    Vector<u32> current_row;
    current_row.resize(right_length + 1);

    for (size_t column = 0; column <= right_length; ++column)
        previous_row[column] = static_cast<u32>(column);

    for (size_t row = 1; row <= left_length; ++row) {
        current_row[0] = static_cast<u32>(row);
        auto minimum_row_distance = current_row[0];

        for (size_t column = 1; column <= right_length; ++column) {
            auto substitution_cost = left_at(row - 1) == right_at(column - 1) ? 0u : 1u;

            auto insertion = current_row[column - 1] + 1;
            auto deletion = previous_row[column] + 1;
            auto substitution = previous_row[column - 1] + substitution_cost;

            auto best_cost = min(insertion, min(deletion, substitution));
            current_row[column] = best_cost;
            minimum_row_distance = min(minimum_row_distance, best_cost);
        }

        if (minimum_row_distance > max_distance)
            return max_distance + 1;

        swap(previous_row, current_row);
    }

    return previous_row[right_length];
}

static u32 bounded_edit_distance(StringView left, StringView right, u32 max_distance)
{
    if (left == right)
        return 0;

    auto left_is_ascii = true;
    for (auto byte : left.bytes()) {
        if (!is_ascii(byte)) {
            left_is_ascii = false;
            break;
        }
    }

    auto right_is_ascii = true;
    for (auto byte : right.bytes()) {
        if (!is_ascii(byte)) {
            right_is_ascii = false;
            break;
        }
    }

    if (left_is_ascii && right_is_ascii) {
        return bounded_edit_distance_impl(
            left.length(),
            right.length(),
            max_distance,
            [&](size_t index) { return static_cast<u32>(left[index]); },
            [&](size_t index) { return static_cast<u32>(right[index]); });
    }

    auto utf8_to_code_points_for_matching = [](StringView text) {
        Vector<u32> code_points;
        auto utf8_view = Utf8View { text };
        if (!utf8_view.validate()) {
            code_points.ensure_capacity(text.length());
            for (auto byte : text.bytes())
                code_points.append(byte);
            return code_points;
        }

        code_points.ensure_capacity(text.length());
        for (auto code_point : utf8_view)
            code_points.append(code_point);
        return code_points;
    };

    auto left_code_points = utf8_to_code_points_for_matching(left);
    auto right_code_points = utf8_to_code_points_for_matching(right);
    return bounded_edit_distance_impl(
        left_code_points.size(),
        right_code_points.size(),
        max_distance,
        [&](size_t index) { return left_code_points[index]; },
        [&](size_t index) { return right_code_points[index]; });
}

static u32 max_typo_distance_for_query(StringView normalized_query)
{
    size_t query_length = normalized_query.length();
    auto utf8_view = Utf8View { normalized_query };
    if (utf8_view.validate()) {
        query_length = 0;
        for (auto code_point : utf8_view)
            (void)code_point, ++query_length;
    }

    if (query_length <= 2)
        return 0;
    // Distance 2 on short queries is too permissive and produces noisy matches.
    if (query_length <= 6)
        return 1;
    return max_typo_distance;
}

static bool query_looks_url_like(StringView normalized_query)
{
    return normalized_query.contains('.') || normalized_query.contains('/') || normalized_query.contains(':');
}

static size_t code_point_length_for_matching(StringView text)
{
    auto utf8_view = Utf8View { text };
    if (!utf8_view.validate())
        return text.length();

    size_t code_point_count = 0;
    for (auto code_point : utf8_view)
        (void)code_point, ++code_point_count;
    return code_point_count;
}

static StringView prefix_for_matching(StringView text, size_t max_code_points)
{
    if (max_code_points == 0)
        return {};

    auto utf8_view = Utf8View { text };
    if (!utf8_view.validate())
        return text.substring_view(0, min(text.length(), max_code_points));

    size_t code_point_count = 0;
    size_t prefix_length_in_bytes = 0;
    for (auto it = utf8_view.begin(); it != utf8_view.end() && code_point_count < max_code_points; ++it, ++code_point_count)
        prefix_length_in_bytes = utf8_view.byte_offset_of(it) + it.underlying_code_point_length_in_bytes();

    return text.substring_view(0, prefix_length_in_bytes);
}

static u32 minimal_distance_to_entry_prefix(StringView query, size_t query_code_point_length, StringView entry, u32 max_distance)
{
    auto entry_code_point_length = code_point_length_for_matching(entry);
    if (entry_code_point_length == 0)
        return max_distance + 1;

    auto min_prefix_code_points = query_code_point_length > max_distance
        ? query_code_point_length - max_distance
        : 1;
    auto max_prefix_code_points = query_code_point_length + max_distance;
    auto last_prefix_code_points = min(max_prefix_code_points, entry_code_point_length);
    auto first_prefix_code_points = min(min_prefix_code_points, last_prefix_code_points);

    auto best_distance = max_distance + 1;
    for (size_t prefix_code_points = first_prefix_code_points; prefix_code_points <= last_prefix_code_points; ++prefix_code_points) {
        auto comparison_view = prefix_for_matching(entry, prefix_code_points);
        if (comparison_view.is_empty())
            continue;

        auto distance = bounded_edit_distance(query, comparison_view, max_distance);
        best_distance = min(best_distance, distance);
        if (best_distance == 0)
            break;
    }

    return best_distance;
}

class LocalSuggestionIndex {
public:
    static LocalSuggestionIndex& the()
    {
        static LocalSuggestionIndex index;
        return index;
    }

    void record(String const& text, SuggestionSource source, SuggestionKind kind, Optional<String> title = {})
    {
        assert_thread_affinity();

        auto search_result_filter = search_result_navigation_filter_from_settings();
        if (kind == SuggestionKind::Navigational && should_skip_search_result_navigation(text.bytes_as_string_view(), search_result_filter))
            return;

        ensure_loaded();

        auto now = UnixDateTime::now().seconds_since_epoch();
        auto normalized_title = normalize_title_for_storage(title.has_value()
                ? Optional<StringView> { title->bytes_as_string_view() }
                : Optional<StringView> {});
        Optional<String> title_to_apply;
        Optional<String> const* title_update = nullptr;
        if (kind == SuggestionKind::Navigational && title.has_value()) {
            if (can_store_title_for_navigational_text(text.bytes_as_string_view()))
                title_to_apply = move(normalized_title);
            title_update = &title_to_apply;
        }

        auto record_single_entry = [&](String const& candidate_entry_text, Optional<String> const* candidate_title) {
            auto normalized_text = normalize_suggestion_text(candidate_entry_text);
            if (normalized_text.is_empty())
                return;
            if (should_exclude_from_local_index(normalized_text))
                return;

            auto key = normalized_text;
            auto existing_entry = m_entries.find(key);
            if (existing_entry == m_entries.end()) {
                auto entry = LocalSuggestionEntry {
                    .text = candidate_entry_text,
                    .title = candidate_title ? *candidate_title : Optional<String> {},
                    .normalized_text = move(normalized_text),
                    .kind = kind,
                    .source = source,
                    .frequency = 1.0,
                    .last_used_unix_seconds = now,
                };
                MUST(m_entries.try_set(key, move(entry)));
                existing_entry = m_entries.find(key);
            } else {
                existing_entry->value.text = candidate_entry_text;
                existing_entry->value.frequency += 1.0;
                existing_entry->value.last_used_unix_seconds = now;
                if (candidate_title)
                    existing_entry->value.title = *candidate_title;

                if (source == SuggestionSource::Bookmark)
                    existing_entry->value.source = source;
                if (kind == SuggestionKind::Navigational)
                    existing_entry->value.kind = kind;
            }

            // Append-only updates; removals are handled by full rebuild.
            note_entries_mutation(false);
            append_entry_to_indexes(existing_entry->key, existing_entry->value, 1);
        };

        auto entry_text = kind == SuggestionKind::Navigational
            ? sanitize_navigational_text_for_storage(text.bytes_as_string_view())
            : MUST(String::from_utf8(text.bytes_as_string_view().trim_whitespace()));
        auto host_only_entry_text = kind == SuggestionKind::Navigational
            ? sanitize_navigational_host_only_for_storage(text.bytes_as_string_view())
            : Optional<String> {};

        if (host_only_entry_text.has_value())
            record_single_entry(host_only_entry_text.value(), nullptr);
        record_single_entry(entry_text, title_update);

        if (prune_entries_to_limit(configured_entry_limit()))
            rebuild_indexes_from_entries();
        persist_to_disk();
    }

    void update_navigation_title(String const& text, String const& title)
    {
        assert_thread_affinity();
        ensure_loaded();

        auto entry_text = sanitize_navigational_text_for_storage(text.bytes_as_string_view());
        if (entry_text.is_empty())
            return;

        auto key = normalize_suggestion_text(entry_text);
        if (key.is_empty())
            return;

        auto existing_entry = m_entries.find(key);
        if (existing_entry == m_entries.end())
            return;
        if (existing_entry->value.kind != SuggestionKind::Navigational)
            return;

        Optional<String> title_to_apply;
        if (can_store_title_for_navigational_text(text.bytes_as_string_view()))
            title_to_apply = normalize_title_for_storage(title.bytes_as_string_view());

        if (existing_entry->value.title == title_to_apply)
            return;

        existing_entry->value.title = move(title_to_apply);
        note_entries_mutation(false);
        append_entry_to_indexes(existing_entry->key, existing_entry->value, 1);
        persist_to_disk();
    }

    void rebuild_from_sources(LocalSuggestionSources sources)
    {
        assert_thread_affinity();
        ensure_loaded();
        m_search_title_data_in_index = configured_search_title_data_enabled();
        auto search_result_filter = search_result_navigation_filter_from_settings();
        apply_entries(build_entries_from_sources(move(sources), configured_entry_limit(), search_result_filter));
        persist_to_disk();
    }

    void set_on_rebuild_state_change(Function<void()> callback)
    {
        assert_thread_affinity();
        m_on_rebuild_state_change = move(callback);
    }

    void schedule_rebuild_after_source_removal()
    {
        schedule_rebuild_after_source_removal({});
    }

    void schedule_rebuild_after_source_removal(Optional<LocalSuggestionSources> sources)
    {
        assert_thread_affinity();
        ensure_loaded();

        auto was_rebuilding = is_rebuild_in_progress();

        ++m_pending_rebuild_generation;
        m_pending_sources_for_rebuild = move(sources);
        m_rebuild_pending = true;

        purge_entries_and_delete_index_file();

        if (!m_rebuild_after_source_removal_timer) {
            m_rebuild_after_source_removal_timer = Core::Timer::create_single_shot(source_removal_rebuild_delay_ms, [this] {
                start_pending_rebuild_now();
            });
        }

        m_rebuild_after_source_removal_timer->restart();

        if (!was_rebuilding)
            notify_rebuild_state_change();
    }

    void notify_omnibox_interaction()
    {
        assert_thread_affinity();
        ensure_loaded();

        if (!m_rebuild_pending)
            return;

        if (m_rebuild_after_source_removal_timer)
            m_rebuild_after_source_removal_timer->stop();

        start_pending_rebuild_now();
    }

    void clear()
    {
        assert_thread_affinity();
        ensure_loaded();

        ++m_pending_rebuild_generation;
        m_rebuild_pending = false;
        m_in_flight_rebuild_generation.clear();
        m_pending_sources_for_rebuild.clear();
        if (m_rebuild_after_source_removal_timer)
            m_rebuild_after_source_removal_timer->stop();

        purge_entries_and_delete_index_file();
        notify_rebuild_state_change();
    }

    void rebuild_indexes_from_current_entries()
    {
        assert_thread_affinity();
        ensure_loaded();
        if (m_load_in_flight)
            return;

        m_search_title_data_in_index = configured_search_title_data_enabled();
        rebuild_indexes_from_entries();
        persist_to_disk();
    }

    void flush_to_disk(Core::EventLoop& event_loop)
    {
        assert_thread_affinity();
        if (!m_load_started && !m_persist_in_flight && !m_pending_serialized_index.has_value())
            return;

        auto wait_for_state_clear = [&](Function<bool()> const& condition, StringView state_name) {
            auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
            while (!condition()) {
                event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
                if (timer.elapsed_milliseconds() >= shutdown_flush_wait_timeout_ms) {
                    warnln("Timed out waiting for autocomplete {} during shutdown flush after {}ms.", state_name, shutdown_flush_wait_timeout_ms);
                    return false;
                }
                (void)Core::System::sleep_ms(1);
            }
            return true;
        };

        if (m_load_in_flight && !wait_for_state_clear([this] { return !m_load_in_flight; }, "load completion"sv))
            warnln("Proceeding with shutdown flush using current in-memory autocomplete entries.");

        if (m_persist_in_flight && !wait_for_state_clear([this] { return !m_persist_in_flight; }, "persist completion"sv)) {
            warnln("Skipping synchronous autocomplete shutdown flush to avoid racing with an in-flight async persist.");
            return;
        }

        m_pending_serialized_index.clear();

        auto path = index_file_path();
        auto temporary_path = temporary_index_file_path();
        auto backup_path = backup_index_file_path();
        auto serialized_index = serialize_entries_for_disk();

        if (auto persist_result = write_serialized_index_to_disk(path, temporary_path, backup_path, serialized_index); persist_result.is_error())
            warnln("Unable to persist autocomplete index during shutdown flush: {}", persist_result.error());
    }

    LocalSuggestionIndexStats stats()
    {
        assert_thread_affinity();
        ensure_loaded();

        LocalSuggestionIndexStats stats;
        stats.total_entries = m_entries.size();
        stats.phrase_prefixes = m_phrase_prefix_index.size();
        stats.token_prefixes = m_token_prefix_index.size();
        stats.term_transition_contexts = m_term_transitions.size();
        stats.is_loading = m_load_in_flight;
        stats.is_loaded = m_load_started && !m_load_in_flight;
        stats.rebuild_pending = m_rebuild_pending;
        stats.rebuild_in_progress = m_in_flight_rebuild_generation.has_value();

        HashTable<String> unique_tokens;

        for (auto const& entry : m_entries) {
            if (entry.value.kind == SuggestionKind::Navigational)
                ++stats.navigational_entries;
            else
                ++stats.query_completion_entries;

            if (entry.value.source == SuggestionSource::Bookmark)
                ++stats.bookmark_entries;
            else if (entry.value.source == SuggestionSource::History)
                ++stats.history_entries;

            auto tokens = tokenize(entry.value.normalized_text);
            for (auto const& token : tokens)
                unique_tokens.set(token);
        }

        for (auto const& transitions : m_term_transitions)
            stats.term_transition_edges += transitions.value.size();

        stats.unique_tokens = unique_tokens.size();
        return stats;
    }

    LocalSuggestionSources sources_after_history_deletion(i64 delete_history_since_unix_seconds)
    {
        assert_thread_affinity();
        ensure_loaded();

        LocalSuggestionSources sources;

        struct HistoryCandidate {
            String text;
            i64 last_used_unix_seconds { 0 };
        };

        Vector<HistoryCandidate> history_candidates;
        history_candidates.ensure_capacity(m_entries.size());
        sources.bookmarks.ensure_capacity(m_entries.size());

        for (auto const& entry : m_entries) {
            if (entry.value.source == SuggestionSource::Bookmark && entry.value.kind == SuggestionKind::Navigational)
                sources.bookmarks.append(entry.value.text);

            if (entry.value.source != SuggestionSource::History)
                continue;
            if (entry.value.last_used_unix_seconds >= delete_history_since_unix_seconds)
                continue;

            history_candidates.append(HistoryCandidate {
                .text = entry.value.text,
                .last_used_unix_seconds = entry.value.last_used_unix_seconds,
            });
        }

        quick_sort(history_candidates, [](auto const& left, auto const& right) {
            if (left.last_used_unix_seconds == right.last_used_unix_seconds)
                return left.text < right.text;
            return left.last_used_unix_seconds > right.last_used_unix_seconds;
        });

        sources.history_newest_first.ensure_capacity(history_candidates.size());
        for (auto& candidate : history_candidates)
            sources.history_newest_first.append(move(candidate.text));

        return sources;
    }

    Vector<AutocompleteSuggestion> query(StringView query, size_t max_results, bool prefer_navigational)
    {
        assert_thread_affinity();
        ensure_loaded();

        if (is_rebuild_in_progress()) {
            Vector<AutocompleteSuggestion> results;
            if (max_results == 0)
                return results;

            results.append(AutocompleteSuggestion {
                .text = MUST(String::from_utf8(local_index_rebuild_placeholder)),
                .title = {},
                .kind = SuggestionKind::QueryCompletion,
                .source = SuggestionSource::History,
                .score = NumericLimits<double>::max(),
            });
            return results;
        }

        if (prune_entries_to_limit(configured_entry_limit())) {
            rebuild_indexes_from_entries();
            persist_to_disk();
        }

        Vector<AutocompleteSuggestion> results;
        if (max_results == 0)
            return results;

        auto normalized_query_info = normalize_query_for_matching(query, prefer_navigational);
        auto normalized_query = move(normalized_query_info.text);
        if (normalized_query.is_empty())
            return normalized_query_info.show_top_navigational_results
                ? top_navigational_results(max_results)
                : results;

        auto query_tokens = tokenize(normalized_query);
        auto query_view = normalized_query.bytes_as_string_view();
        auto query_max_typo_distance = max_typo_distance_for_query(query_view);
        OrderedHashTable<String> candidate_keys;
        auto allow_token_prefix_candidates = !(prefer_navigational && query_looks_url_like(query_view));

        if (auto it = m_phrase_prefix_index.find(normalized_query); it != m_phrase_prefix_index.end()) {
            for (auto const& key : it->value)
                candidate_keys.set(key);
        }

        if (allow_token_prefix_candidates && !query_tokens.is_empty()) {
            if (auto it = m_token_prefix_index.find(query_tokens.last()); it != m_token_prefix_index.end()) {
                for (auto const& key : it->value)
                    candidate_keys.set(key);
            }
        }

        HashMap<String, double> typo_penalty_by_key;

        // Fuzzy recall on low-hit prefixes.
        if (query_max_typo_distance > 0 && candidate_keys.size() < max_results / 2) {
            auto query_code_point_length = code_point_length_for_matching(query_view);
            auto compare_query_with_entry = [&](StringView entry_view) {
                return minimal_distance_to_entry_prefix(query_view, query_code_point_length, entry_view, query_max_typo_distance);
            };

            for (auto const& entry : m_entries) {
                auto entry_view = entry.value.normalized_text.bytes_as_string_view();
                auto distance = compare_query_with_entry(entry_view);
                if (distance > query_max_typo_distance && entry.value.kind == SuggestionKind::Navigational) {
                    if (auto match_view = text_without_common_www_prefix_for_matching(entry_view); match_view.has_value())
                        distance = min(distance, compare_query_with_entry(*match_view));
                }
                if (distance > query_max_typo_distance)
                    continue;

                candidate_keys.set(entry.key);
                typo_penalty_by_key.set(entry.key, static_cast<double>(distance));
            }
        }

        auto now = UnixDateTime::now().seconds_since_epoch();
        results.ensure_capacity(candidate_keys.size());

        for (auto const& key : candidate_keys) {
            auto entry = m_entries.find(key);
            if (entry == m_entries.end())
                continue;

            auto const& value = entry->value;
            auto value_text_view = value.normalized_text.bytes_as_string_view();
            auto age_seconds = max<i64>(0, now - value.last_used_unix_seconds);
            auto age_days = static_cast<double>(age_seconds) / 86400.0;

            auto starts_with_query = value_text_view.starts_with(query_view);
            auto starts_with_query_ignoring_www = false;
            auto contains_query_ignoring_www = false;
            if (value.kind == SuggestionKind::Navigational) {
                if (auto match_text_view = text_without_common_www_prefix_for_matching(value_text_view); match_text_view.has_value()) {
                    starts_with_query_ignoring_www = !starts_with_query && match_text_view->starts_with(query_view);
                    contains_query_ignoring_www = match_text_view->contains(query_view);
                }
            }
            auto contains_query = value_text_view.contains(query_view) || contains_query_ignoring_www;
            auto title_starts_with_query = false;
            auto title_contains_query = false;
            if (m_search_title_data_in_index && value.title.has_value()) {
                auto normalized_title = normalize_suggestion_text(value.title->bytes_as_string_view());
                auto normalized_title_view = normalized_title.bytes_as_string_view();
                title_starts_with_query = normalized_title_view.starts_with(query_view);
                title_contains_query = !title_starts_with_query && normalized_title_view.contains(query_view);
            }
            auto has_text_match = starts_with_query
                || starts_with_query_ignoring_www
                || contains_query
                || title_starts_with_query
                || title_contains_query;
            auto has_typo_match = typo_penalty_by_key.contains(key);
            if (!has_text_match && !has_typo_match)
                continue;

            auto score = 0.0;
            if (starts_with_query || starts_with_query_ignoring_www)
                score += 10.0;
            else if (contains_query)
                score += 3.5;
            else if (title_starts_with_query)
                score += 6.0;
            else if (title_contains_query)
                score += 2.5;

            if (starts_with_query_ignoring_www)
                score += 2.0;

            score += value.frequency * 1.5;
            score += 4.0 / (1.0 + age_days);

            if (prefer_navigational && value.kind == SuggestionKind::Navigational)
                score += 3.0;
            else if (!prefer_navigational && value.kind == SuggestionKind::QueryCompletion)
                score += 1.5;

            if (value.source == SuggestionSource::Bookmark)
                score += 2.0;

            if (auto typo_penalty = typo_penalty_by_key.get(key); typo_penalty.has_value())
                score -= *typo_penalty * 2.0;

            results.unchecked_append(AutocompleteSuggestion {
                .text = value.text,
                .title = value.title,
                .kind = value.kind,
                .source = value.source,
                .score = score,
            });
        }

        // Query continuation using the term-transition index.
        if (query_tokens.size() >= 2) {
            auto const& context = query_tokens[query_tokens.size() - 2];
            auto const& partial = query_tokens.last();
            if (auto transitions = m_term_transitions.get(context); transitions.has_value()) {
                struct CompletionCandidate {
                    String token;
                    u32 count { 0 };
                };

                Vector<CompletionCandidate> next_token_candidates;
                for (auto const& transition : *transitions) {
                    if (!transition.key.starts_with_bytes(partial.bytes_as_string_view()))
                        continue;
                    next_token_candidates.append({ transition.key, transition.value });
                }

                quick_sort(next_token_candidates, [](auto const& left, auto const& right) {
                    if (left.count == right.count)
                        return left.token < right.token;
                    return left.count > right.count;
                });

                auto completion_count = min(next_token_candidates.size(), max_results);
                for (size_t index = 0; index < completion_count; ++index) {
                    StringBuilder builder;
                    for (size_t token_index = 0; token_index < query_tokens.size() - 1; ++token_index) {
                        if (token_index > 0)
                            builder.append(' ');
                        builder.append(query_tokens[token_index]);
                    }
                    if (!builder.is_empty())
                        builder.append(' ');
                    builder.append(next_token_candidates[index].token);

                    auto completion_text = builder.to_string_without_validation();
                    results.append(AutocompleteSuggestion {
                        .text = completion_text,
                        .title = {},
                        .kind = SuggestionKind::QueryCompletion,
                        .source = SuggestionSource::History,
                        .score = 2.0 + next_token_candidates[index].count,
                    });
                }
            }
        }

        quick_sort(results, [](auto const& left, auto const& right) {
            if (left.score == right.score)
                return left.text < right.text;
            return left.score > right.score;
        });

        HashMap<String, size_t> deduped_result_indices;
        Vector<AutocompleteSuggestion> deduped_results;
        deduped_results.ensure_capacity(results.size());

        for (auto& suggestion : results) {
            auto key = normalize_suggestion_text(suggestion.text);
            if (auto existing_index = deduped_result_indices.get(key); existing_index.has_value()) {
                if (suggestion.score > deduped_results[*existing_index].score)
                    deduped_results[*existing_index] = move(suggestion);
                continue;
            }

            deduped_result_indices.set(move(key), deduped_results.size());
            deduped_results.unchecked_append(move(suggestion));

            if (deduped_results.size() >= max_results)
                break;
        }

        return deduped_results;
    }

    Vector<AutocompleteSuggestion> top_navigational_results(size_t max_results) const
    {
        Vector<AutocompleteSuggestion> results;
        if (max_results == 0)
            return results;

        auto now = UnixDateTime::now().seconds_since_epoch();
        for (auto const& entry : m_entries) {
            if (entry.value.kind != SuggestionKind::Navigational)
                continue;

            auto age_seconds = max<i64>(0, now - entry.value.last_used_unix_seconds);
            auto age_days = static_cast<double>(age_seconds) / 86400.0;

            auto score = 0.0;
            score += entry.value.frequency * 1.5;
            score += 4.0 / (1.0 + age_days);
            if (entry.value.source == SuggestionSource::Bookmark)
                score += 2.0;

            results.append(AutocompleteSuggestion {
                .text = entry.value.text,
                .title = entry.value.title,
                .kind = entry.value.kind,
                .source = entry.value.source,
                .score = score,
            });
        }

        quick_sort(results, [](auto const& left, auto const& right) {
            if (left.score == right.score)
                return left.text < right.text;
            return left.score > right.score;
        });

        if (results.size() > max_results)
            results.resize(max_results);
        return results;
    }

private:
    LocalSuggestionIndex() = default;

    void assert_thread_affinity()
    {
#ifndef NDEBUG
        auto* current_thread_event_queue = &Core::ThreadEventQueue::current();
        if (!m_owner_thread_event_queue) {
            m_owner_thread_event_queue = current_thread_event_queue;
            return;
        }

        ASSERT(current_thread_event_queue == m_owner_thread_event_queue);
#endif
    }

    static ByteString index_file_path()
    {
        auto data_directory = ByteString::formatted("{}/Ladybird", Core::StandardPaths::user_data_directory());
        return ByteString::formatted("{}/{}", data_directory, local_index_file_name);
    }

    static ByteString temporary_index_file_path()
    {
        return ByteString::formatted("{}.tmp", index_file_path());
    }

    static ByteString backup_index_file_path()
    {
        return ByteString::formatted("{}.bak", index_file_path());
    }

    static Vector<LocalSuggestionEntry> build_entries_from_sources(LocalSuggestionSources sources, size_t max_entries, SearchResultNavigationFilter const& search_result_filter)
    {
        Vector<LocalSuggestionEntry> entries;
        if (max_entries == 0)
            return entries;

        entries.ensure_capacity(max_entries);
        HashMap<String, size_t> entry_indices_by_normalized_text;
        auto now = UnixDateTime::now().seconds_since_epoch();

        auto add_single_source_entry = [&](String const& candidate_entry_text, SuggestionSource source, SuggestionKind kind, i64 timestamp) {
            auto normalized_text = normalize_suggestion_text(candidate_entry_text);
            if (normalized_text.is_empty())
                return;
            if (should_exclude_from_local_index(normalized_text))
                return;

            if (auto existing_entry_index = entry_indices_by_normalized_text.get(normalized_text); existing_entry_index.has_value()) {
                auto& existing_entry = entries[*existing_entry_index];
                existing_entry.text = candidate_entry_text;
                existing_entry.frequency += 1.0;
                existing_entry.last_used_unix_seconds = max(existing_entry.last_used_unix_seconds, timestamp);

                if (source == SuggestionSource::Bookmark)
                    existing_entry.source = source;
                if (kind == SuggestionKind::Navigational)
                    existing_entry.kind = kind;
                return;
            }

            if (entries.size() >= max_entries)
                return;

            entry_indices_by_normalized_text.set(normalized_text, entries.size());
            entries.append(LocalSuggestionEntry {
                .text = candidate_entry_text,
                .title = {},
                .normalized_text = move(normalized_text),
                .kind = kind,
                .source = source,
                .frequency = 1.0,
                .last_used_unix_seconds = timestamp,
            });
        };

        auto add_source_entry = [&](String const& text, SuggestionSource source, SuggestionKind kind, i64 timestamp) {
            if (kind == SuggestionKind::Navigational && should_skip_search_result_navigation(text.bytes_as_string_view(), search_result_filter))
                return;

            auto entry_text = kind == SuggestionKind::Navigational
                ? sanitize_navigational_text_for_storage(text.bytes_as_string_view())
                : MUST(String::from_utf8(text.bytes_as_string_view().trim_whitespace()));
            auto host_only_entry_text = kind == SuggestionKind::Navigational
                ? sanitize_navigational_host_only_for_storage(text.bytes_as_string_view())
                : Optional<String> {};

            if (host_only_entry_text.has_value())
                add_single_source_entry(host_only_entry_text.release_value(), source, kind, timestamp);
            add_single_source_entry(entry_text, source, kind, timestamp);
        };

        // Keep bookmarks first.
        for (auto const& bookmark : sources.bookmarks) {
            if (entries.size() >= max_entries)
                break;
            add_source_entry(bookmark, SuggestionSource::Bookmark, SuggestionKind::Navigational, now);
        }

        // Then take history from newest to oldest until the cap is reached.
        auto history_timestamp = now - 1;
        for (auto const& history_entry : sources.history_newest_first) {
            if (entries.size() >= max_entries)
                break;

            auto kind = looks_like_navigational(history_entry.bytes_as_string_view()) ? SuggestionKind::Navigational : SuggestionKind::QueryCompletion;
            add_source_entry(history_entry, SuggestionSource::History, kind, history_timestamp);
            --history_timestamp;
        }

        return entries;
    }

    void apply_entries(Vector<LocalSuggestionEntry> entries)
    {
        assert_thread_affinity();
        if (!m_entries.is_empty() || !entries.is_empty())
            note_entries_mutation(true);

        m_entries.clear();
        m_phrase_prefix_index.clear();
        m_token_prefix_index.clear();
        m_term_transitions.clear();

        for (auto& entry : entries) {
            auto key = entry.normalized_text;
            m_entries.set(key, move(entry));
        }

        rebuild_indexes_from_entries();
    }

    bool is_rebuild_in_progress() const
    {
        return m_rebuild_pending || m_in_flight_rebuild_generation.has_value();
    }

    void start_pending_rebuild_now()
    {
        assert_thread_affinity();
        if (!m_rebuild_pending || m_in_flight_rebuild_generation.has_value())
            return;

        auto generation = m_pending_rebuild_generation;
        Optional<LocalSuggestionSources> sources;
        if (m_pending_sources_for_rebuild.has_value())
            sources = m_pending_sources_for_rebuild.release_value();
        auto max_entries = configured_entry_limit();
        auto search_title_data_enabled = configured_search_title_data_enabled();
        auto search_result_filter = search_result_navigation_filter_from_settings();

        m_rebuild_pending = false;
        m_in_flight_rebuild_generation = generation;
        notify_rebuild_state_change();

        if (!sources.has_value()) {
            finish_pending_rebuild(generation, Optional<Vector<LocalSuggestionEntry>> {}, {});
            return;
        }

        (void)Threading::BackgroundAction<Vector<LocalSuggestionEntry>>::construct(
            [sources = sources.release_value(), max_entries, search_result_filter](Threading::BackgroundAction<Vector<LocalSuggestionEntry>>&) mutable -> ErrorOr<Vector<LocalSuggestionEntry>> {
                return build_entries_from_sources(move(sources), max_entries, search_result_filter);
            },
            [this, generation, search_title_data_enabled](Vector<LocalSuggestionEntry> rebuilt_entries) -> ErrorOr<void> {
                assert_thread_affinity();
                finish_pending_rebuild(generation, move(rebuilt_entries), search_title_data_enabled);
                return {};
            },
            [this, generation](Error error) {
                assert_thread_affinity();
                if (!m_in_flight_rebuild_generation.has_value() || *m_in_flight_rebuild_generation != generation)
                    return;

                // If a newer purge request exists, this result is stale and should be ignored silently.
                if (generation == m_pending_rebuild_generation)
                    warnln("Unable to rebuild autocomplete index: {}", error);

                m_in_flight_rebuild_generation.clear();
                notify_rebuild_state_change();

                if (m_rebuild_pending && (!m_rebuild_after_source_removal_timer || !m_rebuild_after_source_removal_timer->is_active()))
                    start_pending_rebuild_now();
            });
    }

    void finish_pending_rebuild(u64 generation, Optional<Vector<LocalSuggestionEntry>> rebuilt_entries, Optional<bool> search_title_data_enabled)
    {
        assert_thread_affinity();
        if (!m_in_flight_rebuild_generation.has_value() || *m_in_flight_rebuild_generation != generation)
            return;

        m_in_flight_rebuild_generation.clear();

        if (generation == m_pending_rebuild_generation && rebuilt_entries.has_value()) {
            if (search_title_data_enabled.has_value())
                m_search_title_data_in_index = *search_title_data_enabled;
            apply_entries(rebuilt_entries.release_value());
            persist_to_disk();
        }

        notify_rebuild_state_change();

        if (m_rebuild_pending && (!m_rebuild_after_source_removal_timer || !m_rebuild_after_source_removal_timer->is_active()))
            start_pending_rebuild_now();
    }

    void purge_entries_and_delete_index_file()
    {
        assert_thread_affinity();
        if (!m_entries.is_empty() || !m_phrase_prefix_index.is_empty() || !m_token_prefix_index.is_empty() || !m_term_transitions.is_empty())
            note_entries_mutation(true);

        m_entries.clear();
        m_phrase_prefix_index.clear();
        m_token_prefix_index.clear();
        m_term_transitions.clear();

        ++m_purge_generation;
        m_pending_serialized_index.clear();

        auto path = index_file_path();
        if (auto unlink_result = Core::System::unlink(path); unlink_result.is_error()) {
            if (!unlink_result.error().is_errno() || unlink_result.error().code() != ENOENT)
                warnln("Unable to delete autocomplete index file '{}': {}", path, unlink_result.error());
        }

        auto temporary_path = temporary_index_file_path();
        if (auto unlink_result = Core::System::unlink(temporary_path); unlink_result.is_error()) {
            if (!unlink_result.error().is_errno() || unlink_result.error().code() != ENOENT)
                warnln("Unable to delete autocomplete temporary file '{}': {}", temporary_path, unlink_result.error());
        }

        auto backup_path = backup_index_file_path();
        if (auto unlink_result = Core::System::unlink(backup_path); unlink_result.is_error()) {
            if (!unlink_result.error().is_errno() || unlink_result.error().code() != ENOENT)
                warnln("Unable to delete autocomplete backup file '{}': {}", backup_path, unlink_result.error());
        }
    }

    void notify_rebuild_state_change()
    {
        assert_thread_affinity();
        if (m_on_rebuild_state_change)
            m_on_rebuild_state_change();
    }

    void ensure_loaded()
    {
        assert_thread_affinity();
        if (m_load_started)
            return;

        m_load_started = true;
        m_load_in_flight = true;
        m_load_start_entries_version = m_entries_version;
        m_destructive_mutation_since_load_started = false;
        auto load_generation = ++m_load_generation;
        auto configured_limit = configured_entry_limit();
        auto desired_loader_cap = max(configured_limit, static_cast<size_t>(1));
        if (desired_loader_cap < local_index_loader_absolute_entry_cap)
            desired_loader_cap = min(local_index_loader_absolute_entry_cap, desired_loader_cap * 2);
        auto loader_entry_cap = min(local_index_loader_absolute_entry_cap, desired_loader_cap);

        (void)Threading::BackgroundAction<LoadedLocalSuggestionIndex>::construct(
            [path = index_file_path(), temporary_path = temporary_index_file_path(), backup_path = backup_index_file_path(), loader_entry_cap](Threading::BackgroundAction<LoadedLocalSuggestionIndex>&) -> ErrorOr<LoadedLocalSuggestionIndex> {
                enum class LoadResultStatus {
                    Loaded,
                    Missing,
                    Failed,
                };

                struct LoadAttemptResult {
                    LoadResultStatus status;
                    LoadedLocalSuggestionIndex loaded_index;
                };

                auto parse_entries = [&](JsonArray const& json_entries, ByteString const& source_path) {
                    auto capped_entry_count = min(json_entries.size(), loader_entry_cap);
                    if (json_entries.size() > loader_entry_cap)
                        warnln("Autocomplete index file '{}' contains {} entries; only loading first {}", source_path, json_entries.size(), loader_entry_cap);

                    Vector<LocalSuggestionEntry> parsed_entries;
                    parsed_entries.ensure_capacity(capped_entry_count);
                    HashMap<String, size_t> parsed_entry_indices_by_normalized_text;

                    size_t scanned_entries = 0;
                    for (auto const& value : json_entries.values()) {
                        if (scanned_entries >= capped_entry_count)
                            break;
                        ++scanned_entries;

                        if (!value.is_object())
                            continue;

                        auto text = value.as_object().get_string("text"sv);
                        auto frequency = value.as_object().get_double_with_precision_loss("frequency"sv);
                        auto last_used_unix_seconds = value.as_object().get_integer<i64>("lastUsedUnixSeconds"sv);
                        auto source = value.as_object().get_integer<u8>("source"sv);
                        auto kind = value.as_object().get_integer<u8>("kind"sv);

                        if (!text.has_value() || !frequency.has_value() || !last_used_unix_seconds.has_value())
                            continue;
                        if (!source.has_value() || !kind.has_value())
                            continue;

                        if (source.value() > to_underlying(SuggestionSource::Remote))
                            continue;
                        if (kind.value() > to_underlying(SuggestionKind::QueryCompletion))
                            continue;

                        auto entry_kind = static_cast<SuggestionKind>(*kind);
                        auto entry_source = static_cast<SuggestionSource>(*source);

                        auto entry_text = text.release_value();
                        if (entry_kind == SuggestionKind::Navigational)
                            entry_text = sanitize_navigational_text_for_storage(entry_text.bytes_as_string_view());
                        auto title_from_file = value.as_object().get_string("title"sv);
                        auto entry_title = normalize_title_for_storage(title_from_file.has_value()
                                ? Optional<StringView> { title_from_file->bytes_as_string_view() }
                                : Optional<StringView> {});
                        if (entry_kind != SuggestionKind::Navigational)
                            entry_title.clear();

                        auto entry_normalized_text = normalize_suggestion_text(entry_text);
                        if (entry_normalized_text.is_empty())
                            continue;
                        if (should_exclude_from_local_index(entry_normalized_text))
                            continue;

                        if (auto existing_entry_index = parsed_entry_indices_by_normalized_text.get(entry_normalized_text); existing_entry_index.has_value()) {
                            auto& existing_entry = parsed_entries[*existing_entry_index];
                            existing_entry.frequency += *frequency;
                            if (*last_used_unix_seconds >= existing_entry.last_used_unix_seconds) {
                                existing_entry.text = entry_text;
                                existing_entry.title = entry_title;
                            }
                            existing_entry.last_used_unix_seconds = max(existing_entry.last_used_unix_seconds, *last_used_unix_seconds);
                            if (entry_source == SuggestionSource::Bookmark)
                                existing_entry.source = entry_source;
                            if (entry_kind == SuggestionKind::Navigational)
                                existing_entry.kind = entry_kind;
                            continue;
                        }

                        parsed_entry_indices_by_normalized_text.set(entry_normalized_text, parsed_entries.size());
                        parsed_entries.unchecked_append(LocalSuggestionEntry {
                            .text = move(entry_text),
                            .title = move(entry_title),
                            .normalized_text = move(entry_normalized_text),
                            .kind = entry_kind,
                            .source = entry_source,
                            .frequency = *frequency,
                            .last_used_unix_seconds = *last_used_unix_seconds,
                        });
                    }

                    if (json_entries.size() == 0)
                        warnln("Autocomplete index file '{}' contains no entries.", source_path);
                    else if (parsed_entries.is_empty())
                        warnln("Autocomplete index file '{}' did not yield any valid entries after validation.", source_path);

                    return parsed_entries;
                };

                auto load_entries_from_file = [&](ByteString const& candidate_path, StringView label) -> LoadAttemptResult {
                    auto file = Core::File::open(candidate_path, Core::File::OpenMode::Read);
                    if (file.is_error()) {
                        if (file.error().is_errno() && file.error().code() == ENOENT)
                            return { LoadResultStatus::Missing, {} };

                        warnln("Unable to read autocomplete {} file '{}': {}", label, candidate_path, file.error());
                        return { LoadResultStatus::Failed, {} };
                    }

                    auto file_contents = file.value()->read_until_eof();
                    if (file_contents.is_error()) {
                        warnln("Unable to read contents of autocomplete {} file '{}': {}", label, candidate_path, file_contents.error());
                        return { LoadResultStatus::Failed, {} };
                    }

                    if (file_contents.value().is_empty()) {
                        warnln("Autocomplete {} file '{}' is empty.", label, candidate_path);
                        return { LoadResultStatus::Failed, {} };
                    }

                    auto parsed_json = JsonValue::from_string(file_contents.value());
                    if (parsed_json.is_error()) {
                        warnln("Unable to parse autocomplete {} file '{}': {}", label, candidate_path, parsed_json.error());
                        return { LoadResultStatus::Failed, {} };
                    }
                    if (!parsed_json.value().is_object()) {
                        warnln("Autocomplete {} file '{}' is invalid: root JSON value is not an object.", label, candidate_path);
                        return { LoadResultStatus::Failed, {} };
                    }

                    auto entries = parsed_json.value().as_object().get_array("entries"sv);
                    if (!entries.has_value()) {
                        warnln("Autocomplete {} file '{}' is invalid: missing 'entries' array.", label, candidate_path);
                        return { LoadResultStatus::Failed, {} };
                    }

                    auto search_title_data_indexed = parsed_json.value().as_object().get_bool(local_index_search_title_data_key).value_or(false);
                    return { LoadResultStatus::Loaded, LoadedLocalSuggestionIndex { .entries = parse_entries(entries.value(), candidate_path), .search_title_data_indexed = search_title_data_indexed } };
                };

                auto index_load_result = load_entries_from_file(path, "index"sv);
                if (index_load_result.status == LoadResultStatus::Loaded)
                    return move(index_load_result.loaded_index);

                if (index_load_result.status == LoadResultStatus::Failed)
                    warnln("Autocomplete index loader: trying to recover from temporary file '{}'.", temporary_path);

                auto temporary_load_result = load_entries_from_file(temporary_path, "temporary index"sv);
                if (temporary_load_result.status == LoadResultStatus::Loaded) {
                    warnln("Autocomplete index loader: recovered entries from temporary file '{}'.", temporary_path);
                    return move(temporary_load_result.loaded_index);
                }

                if (index_load_result.status == LoadResultStatus::Failed || temporary_load_result.status == LoadResultStatus::Failed)
                    warnln("Autocomplete index loader: trying to recover from backup file '{}'.", backup_path);

                auto backup_load_result = load_entries_from_file(backup_path, "backup index"sv);
                if (backup_load_result.status == LoadResultStatus::Loaded) {
                    warnln("Autocomplete index loader: recovered entries from backup file '{}'.", backup_path);
                    return move(backup_load_result.loaded_index);
                }

                if (index_load_result.status == LoadResultStatus::Failed
                    || temporary_load_result.status == LoadResultStatus::Failed
                    || backup_load_result.status == LoadResultStatus::Failed) {
                    warnln("Autocomplete index loader: unable to read local index entries from '{}', '{}', or '{}'.", path, temporary_path, backup_path);
                } else {
                    warnln("Autocomplete index loader: no index entries found in '{}', '{}', or '{}'.", path, temporary_path, backup_path);
                }

                return LoadedLocalSuggestionIndex {};
            },
            [this, load_generation](LoadedLocalSuggestionIndex loaded_index) -> ErrorOr<void> {
                assert_thread_affinity();
                if (load_generation != m_load_generation)
                    return {};

                m_load_in_flight = false;
                auto loaded_entries = move(loaded_index.entries);

                if (m_entries_version == m_load_start_entries_version) {
                    if (!m_entries.is_empty() || !loaded_entries.is_empty())
                        note_entries_mutation(true);

                    m_entries.clear();
                    for (auto& entry : loaded_entries)
                        m_entries.set(entry.normalized_text, move(entry));
                    m_search_title_data_in_index = loaded_index.search_title_data_indexed;

                    auto did_prune = prune_entries_to_limit(configured_entry_limit());
                    rebuild_indexes_from_entries();

                    if (did_prune)
                        persist_to_disk();

                    return {};
                }

                if (m_destructive_mutation_since_load_started)
                    return {};

                auto title_search_mode_changed = m_search_title_data_in_index != loaded_index.search_title_data_indexed;
                m_search_title_data_in_index = loaded_index.search_title_data_indexed;

                auto did_merge = false;
                for (auto& loaded_entry : loaded_entries) {
                    auto existing_entry = m_entries.find(loaded_entry.normalized_text);
                    if (existing_entry == m_entries.end()) {
                        m_entries.set(loaded_entry.normalized_text, move(loaded_entry));
                        did_merge = true;
                        continue;
                    }

                    auto loaded_entry_is_newer = loaded_entry.last_used_unix_seconds >= existing_entry->value.last_used_unix_seconds;
                    if (loaded_entry_is_newer) {
                        existing_entry->value.text = loaded_entry.text;
                        existing_entry->value.title = loaded_entry.title;
                    } else if (!existing_entry->value.title.has_value() && loaded_entry.title.has_value()) {
                        existing_entry->value.title = loaded_entry.title;
                    }

                    existing_entry->value.frequency += loaded_entry.frequency;
                    existing_entry->value.last_used_unix_seconds = max(existing_entry->value.last_used_unix_seconds, loaded_entry.last_used_unix_seconds);
                    if (loaded_entry.source == SuggestionSource::Bookmark)
                        existing_entry->value.source = SuggestionSource::Bookmark;
                    if (loaded_entry.kind == SuggestionKind::Navigational)
                        existing_entry->value.kind = SuggestionKind::Navigational;
                    did_merge = true;
                }

                if (!did_merge && !title_search_mode_changed)
                    return {};

                if (did_merge)
                    note_entries_mutation(false);
                prune_entries_to_limit(configured_entry_limit());
                rebuild_indexes_from_entries();
                if (did_merge) {
                    // Persist merged in-memory state so it survives restart.
                    persist_to_disk();
                }

                return {};
            },
            [this, load_generation](Error error) {
                assert_thread_affinity();
                if (load_generation != m_load_generation)
                    return;

                m_load_in_flight = false;
                warnln("Unable to load autocomplete index: {}", error);
            });
    }

    static size_t configured_entry_limit()
    {
        return Application::settings().autocomplete_local_index_max_entries();
    }

    static bool configured_search_title_data_enabled()
    {
        return Application::settings().autocomplete_search_title_data();
    }

    ByteString serialize_entries_for_disk() const
    {
        JsonArray entries;
        entries.ensure_capacity(m_entries.size());

        for (auto const& entry : m_entries) {
            JsonObject json_entry;
            json_entry.set("text"sv, entry.value.text);
            if (entry.value.title.has_value())
                json_entry.set("title"sv, *entry.value.title);
            json_entry.set("frequency"sv, entry.value.frequency);
            json_entry.set("lastUsedUnixSeconds"sv, entry.value.last_used_unix_seconds);
            json_entry.set("source"sv, to_underlying(entry.value.source));
            json_entry.set("kind"sv, to_underlying(entry.value.kind));

            entries.must_append(move(json_entry));
        }

        JsonObject root;
        root.set("entries"sv, move(entries));
        root.set(local_index_search_title_data_key, m_search_title_data_in_index);
        return root.serialized().to_byte_string();
    }

    static ErrorOr<void> write_serialized_index_to_disk(ByteString const& path, ByteString const& temporary_path, ByteString const& backup_path, ByteString const& serialized_index)
    {
        auto directory = LexicalPath { path }.parent();
        TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes, local_index_directory_permissions));
#if !defined(AK_OS_WINDOWS)
        TRY(Core::System::chmod(directory.string(), local_index_directory_permissions));
#endif

        auto file = TRY(Core::File::open(temporary_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate, local_index_file_permissions));
#if !defined(AK_OS_WINDOWS)
        TRY(Core::System::fchmod(file->fd(), local_index_file_permissions));
#endif
        TRY(file->write_until_depleted(serialized_index.bytes()));
        file->close();

#if defined(AK_OS_WINDOWS)
        auto copy_file_contents = [](ByteString const& source_path, ByteString const& destination_path, bool ignore_missing_source) -> ErrorOr<bool> {
            auto source_file = Core::File::open(source_path, Core::File::OpenMode::Read);
            if (source_file.is_error()) {
                if (ignore_missing_source && source_file.error().is_errno() && source_file.error().code() == ENOENT)
                    return false;
                return source_file.release_error();
            }

            auto source_contents = TRY(source_file.value()->read_until_eof());
            auto destination_file = TRY(Core::File::open(destination_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate, local_index_file_permissions));
            TRY(destination_file->write_until_depleted(source_contents.bytes()));
            destination_file->close();
            return true;
        };

        auto moved_current_file_to_backup = TRY(copy_file_contents(path, backup_path, true));
        if (auto replace_result = copy_file_contents(temporary_path, path, false); replace_result.is_error()) {
            if (moved_current_file_to_backup) {
                if (auto restore_result = copy_file_contents(backup_path, path, false); restore_result.is_error())
                    warnln("Unable to restore previous autocomplete index from backup '{}' to '{}': {}", backup_path, path, restore_result.error());
            }
            return replace_result.release_error();
        }

        if (auto unlink_result = Core::System::unlink(temporary_path); unlink_result.is_error() && (!unlink_result.error().is_errno() || unlink_result.error().code() != ENOENT))
            warnln("Unable to remove temporary autocomplete index file '{}': {}", temporary_path, unlink_result.error());
#else
        auto moved_current_file_to_backup = false;
        if (auto rename_result = Core::System::rename(path, backup_path); rename_result.is_error()) {
            if (!rename_result.error().is_errno() || rename_result.error().code() != ENOENT)
                return rename_result.release_error();
        } else {
            moved_current_file_to_backup = true;
        }

        if (auto rename_result = Core::System::rename(temporary_path, path); rename_result.is_error()) {
            if (moved_current_file_to_backup) {
                if (auto restore_result = Core::System::rename(backup_path, path); restore_result.is_error())
                    warnln("Unable to restore previous autocomplete index from backup '{}' to '{}': {}", backup_path, path, restore_result.error());
            }
            return rename_result.release_error();
        }

        TRY(Core::System::chmod(path, local_index_file_permissions));
#endif
        return {};
    }

    void persist_to_disk()
    {
        assert_thread_affinity();
        m_pending_serialized_index = serialize_entries_for_disk();
        maybe_start_async_persist();
    }

    void maybe_start_async_persist()
    {
        assert_thread_affinity();
        if (m_persist_in_flight || !m_pending_serialized_index.has_value())
            return;

        m_persist_in_flight = true;

        auto serialized_index = m_pending_serialized_index.release_value();
        auto path = index_file_path();
        auto temporary_path = temporary_index_file_path();
        auto backup_path = backup_index_file_path();
        auto path_for_callback = path;
        auto temporary_path_for_callback = temporary_path;
        auto backup_path_for_callback = backup_path;
        auto persist_generation = m_purge_generation;

        (void)Threading::BackgroundAction<Empty>::construct(
            [path = move(path), temporary_path = move(temporary_path), backup_path = move(backup_path), serialized_index = move(serialized_index)](Threading::BackgroundAction<Empty>&) -> ErrorOr<Empty> {
                TRY(write_serialized_index_to_disk(path, temporary_path, backup_path, serialized_index));
                return {};
            },
            [this, path_for_callback = move(path_for_callback), temporary_path_for_callback = move(temporary_path_for_callback), backup_path_for_callback = move(backup_path_for_callback), persist_generation](Empty) -> ErrorOr<void> {
                assert_thread_affinity();
                m_persist_in_flight = false;

                if (persist_generation != m_purge_generation) {
                    if (auto unlink_result = Core::System::unlink(path_for_callback); unlink_result.is_error()) {
                        if (!unlink_result.error().is_errno() || unlink_result.error().code() != ENOENT)
                            warnln("Unable to delete stale autocomplete index file '{}': {}", path_for_callback, unlink_result.error());
                    }

                    if (auto unlink_result = Core::System::unlink(temporary_path_for_callback); unlink_result.is_error()) {
                        if (!unlink_result.error().is_errno() || unlink_result.error().code() != ENOENT)
                            warnln("Unable to delete stale autocomplete temporary file '{}': {}", temporary_path_for_callback, unlink_result.error());
                    }

                    if (auto unlink_result = Core::System::unlink(backup_path_for_callback); unlink_result.is_error()) {
                        if (!unlink_result.error().is_errno() || unlink_result.error().code() != ENOENT)
                            warnln("Unable to delete stale autocomplete backup file '{}': {}", backup_path_for_callback, unlink_result.error());
                    }
                }

                maybe_start_async_persist();
                return {};
            },
            [this, temporary_path_for_callback = move(temporary_path_for_callback), persist_generation](Error error) {
                assert_thread_affinity();
                warnln("Unable to persist autocomplete index: {}", error);
                m_persist_in_flight = false;

                if (auto unlink_result = Core::System::unlink(temporary_path_for_callback); unlink_result.is_error()) {
                    if (!unlink_result.error().is_errno() || unlink_result.error().code() != ENOENT)
                        warnln("Unable to delete autocomplete temporary file '{}': {}", temporary_path_for_callback, unlink_result.error());
                }

                if (persist_generation != m_purge_generation)
                    return;

                maybe_start_async_persist();
            });
    }

    void rebuild_indexes_from_entries()
    {
        assert_thread_affinity();
        m_phrase_prefix_index.clear();
        m_token_prefix_index.clear();
        m_term_transitions.clear();

        for (auto const& entry : m_entries) {
            auto frequency = static_cast<u32>(entry.value.frequency);
            append_entry_to_indexes(entry.key, entry.value, max(1u, frequency));
        }
    }

    bool prune_entries_to_limit(size_t max_entries)
    {
        assert_thread_affinity();
        if (m_entries.size() <= max_entries)
            return false;

        struct EvictionCandidate {
            String key;
            double frequency { 0 };
            i64 last_used_unix_seconds { 0 };
        };

        Vector<EvictionCandidate> candidates;
        candidates.ensure_capacity(m_entries.size());

        for (auto const& entry : m_entries) {
            candidates.unchecked_append(EvictionCandidate {
                .key = entry.key,
                .frequency = entry.value.frequency,
                .last_used_unix_seconds = entry.value.last_used_unix_seconds,
            });
        }

        // Prefer evicting least-used and oldest entries first.
        quick_sort(candidates, [](auto const& left, auto const& right) {
            if (left.frequency == right.frequency) {
                if (left.last_used_unix_seconds == right.last_used_unix_seconds)
                    return left.key < right.key;
                return left.last_used_unix_seconds < right.last_used_unix_seconds;
            }
            return left.frequency < right.frequency;
        });

        auto entries_to_remove = m_entries.size() - max_entries;
        for (size_t index = 0; index < entries_to_remove; ++index)
            m_entries.remove(candidates[index].key);

        note_entries_mutation(true);
        return true;
    }

    void note_entries_mutation(bool destructive)
    {
        ++m_entries_version;
        if (m_load_in_flight && destructive)
            m_destructive_mutation_since_load_started = true;
    }

    void append_entry_to_indexes(String const& entry_key, LocalSuggestionEntry const& entry, u32 weight)
    {
        auto add_phrase_prefixes = [&](StringView phrase_view) {
            auto utf8_phrase_view = Utf8View { phrase_view };
            if (!utf8_phrase_view.validate())
                return;

            size_t prefix_count = 0;
            for (auto it = utf8_phrase_view.begin(); it != utf8_phrase_view.end() && prefix_count < max_index_prefix_length; ++it, ++prefix_count) {
                auto prefix_length_in_bytes = utf8_phrase_view.byte_offset_of(it) + it.underlying_code_point_length_in_bytes();
                auto prefix = MUST(String::from_utf8(phrase_view.substring_view(0, prefix_length_in_bytes)));
                m_phrase_prefix_index.ensure(prefix).set(entry_key);
            }
        };

        auto add_token_prefixes = [&](StringView text) {
            auto tokens = tokenize(text);
            for (auto const& token : tokens) {
                auto token_view = token.bytes_as_string_view();
                auto token_prefix_length = min(max_index_prefix_length, token_view.length());
                for (size_t index = 1; index <= token_prefix_length; ++index) {
                    auto prefix = MUST(String::from_utf8(token_view.substring_view(0, index)));
                    m_token_prefix_index.ensure(prefix).set(entry_key);
                }
            }
        };

        auto phrase_view = entry.normalized_text.bytes_as_string_view();
        add_phrase_prefixes(phrase_view);
        auto phrase_view_without_scheme = text_without_url_scheme_for_matching(phrase_view);
        if (phrase_view_without_scheme != phrase_view && !phrase_view_without_scheme.is_empty())
            add_phrase_prefixes(phrase_view_without_scheme);

        if (entry.kind == SuggestionKind::Navigational) {
            if (auto navigational_match_view = text_without_common_www_prefix_for_matching(phrase_view); navigational_match_view.has_value() && !navigational_match_view->is_empty())
                add_phrase_prefixes(*navigational_match_view);
        }

        add_token_prefixes(entry.normalized_text.bytes_as_string_view());

        if (m_search_title_data_in_index && entry.title.has_value()) {
            auto normalized_title = normalize_suggestion_text(entry.title->bytes_as_string_view());
            auto normalized_title_view = normalized_title.bytes_as_string_view();
            if (!normalized_title_view.is_empty()) {
                add_phrase_prefixes(normalized_title_view);
                add_token_prefixes(normalized_title_view);

                auto title_keywords = title_keywords_for_indexing(normalized_title_view);
                for (auto const& title_keyword : title_keywords) {
                    auto keyword_view = title_keyword.bytes_as_string_view();
                    add_phrase_prefixes(keyword_view);
                    add_token_prefixes(keyword_view);
                }
            }
        }

        auto tokens = tokenize(entry.normalized_text);
        if (tokens.size() < 2)
            return;

        for (size_t index = 0; index + 1 < tokens.size(); ++index) {
            auto& transitions = m_term_transitions.ensure(tokens[index]);
            auto& count = transitions.ensure(tokens[index + 1]);
            count += weight;
        }
    }

    bool m_load_started { false };
    bool m_load_in_flight { false };
    u64 m_load_generation { 0 };
    u64 m_load_start_entries_version { 0 };
    u64 m_entries_version { 0 };
    bool m_destructive_mutation_since_load_started { false };

    HashMap<String, LocalSuggestionEntry> m_entries;
    HashMap<String, OrderedHashTable<String>> m_phrase_prefix_index;
    HashMap<String, OrderedHashTable<String>> m_token_prefix_index;
    HashMap<String, HashMap<String, u32>> m_term_transitions;
    bool m_search_title_data_in_index { false };

    Optional<ByteString> m_pending_serialized_index;
    bool m_persist_in_flight { false };
    u64 m_purge_generation { 0 };

    Optional<LocalSuggestionSources> m_pending_sources_for_rebuild;
    bool m_rebuild_pending { false };
    Optional<u64> m_in_flight_rebuild_generation;
    u64 m_pending_rebuild_generation { 0 };
    RefPtr<Core::Timer> m_rebuild_after_source_removal_timer;
    Function<void()> m_on_rebuild_state_change;
#ifndef NDEBUG
    Core::ThreadEventQueue* m_owner_thread_event_queue { nullptr };
#endif
};

ReadonlySpan<AutocompleteEngine> autocomplete_engines()
{
    return builtin_autocomplete_engines;
}

Optional<AutocompleteEngine const&> find_autocomplete_engine_by_name(StringView name)
{
    return find_value(builtin_autocomplete_engines, [&](auto const& engine) {
        return engine.name == name;
    });
}

Autocomplete::Autocomplete()
{
    m_next_live_instance = s_first_live_autocomplete_instance;
    if (m_next_live_instance)
        m_next_live_instance->m_previous_live_instance = this;
    s_first_live_autocomplete_instance = this;

    LocalSuggestionIndex::the().set_on_rebuild_state_change(Autocomplete::notify_instances_about_local_index_state_change);
}

Autocomplete::~Autocomplete()
{
    if (m_previous_live_instance)
        m_previous_live_instance->m_next_live_instance = m_next_live_instance;
    else
        s_first_live_autocomplete_instance = m_next_live_instance;

    if (m_next_live_instance)
        m_next_live_instance->m_previous_live_instance = m_previous_live_instance;

    m_previous_live_instance = nullptr;
    m_next_live_instance = nullptr;

    if (!s_first_live_autocomplete_instance)
        LocalSuggestionIndex::the().set_on_rebuild_state_change({});
}

void Autocomplete::query_suggestions(String query, SuggestionOptions options)
{
    if (m_request) {
        m_request->stop();
        m_request.clear();
    }

    ++m_query_sequence_number;
    auto const query_sequence_number = m_query_sequence_number;

    m_last_query_options = options;
    m_has_active_query = false;
    m_showing_local_index_rebuild_placeholder = false;
    m_query = move(query);

    notify_omnibox_interaction();

    auto trimmed_query = m_query.bytes_as_string_view().trim_whitespace();
    if (trimmed_query.is_empty() || trimmed_query.starts_with(file_url_prefix)) {
        invoke_suggestions_query_complete({});
        return;
    }

    m_has_active_query = true;

    auto prefer_navigational = looks_like_navigational(trimmed_query);
    auto local_suggestions = LocalSuggestionIndex::the().query(trimmed_query, options.max_results, prefer_navigational);
    m_showing_local_index_rebuild_placeholder = local_suggestions.size() == 1
        && local_suggestions.first().text == local_index_rebuild_placeholder;
    invoke_suggestions_query_complete(local_suggestions);

    if (m_showing_local_index_rebuild_placeholder)
        return;

    if (!options.remote_enabled)
        return;

    auto engine = Application::settings().autocomplete_engine();
    if (!engine.has_value())
        return;

    auto url_string = MUST(String::formatted(engine->query_url, URL::percent_encode(trimmed_query)));
    auto url = URL::Parser::basic_parse(url_string);
    if (!url.has_value())
        return;

    m_request = Application::request_server_client().start_request("GET"sv, *url);

    m_request->set_buffered_request_finished_callback(
        [this,
            engine = engine.release_value(),
            query_sequence_number,
            local_suggestions = move(local_suggestions),
            query = m_query,
            prefer_navigational,
            max_results = options.max_results](u64,
            Requests::RequestTimingInfo const&,
            Optional<Requests::NetworkError> const& network_error,
            HTTP::HeaderList const& response_headers,
            Optional<u32> response_code,
            Optional<String> const& reason_phrase,
            ReadonlyBytes payload) mutable {
            Core::deferred_invoke([this]() { m_request.clear(); });

            if (query_sequence_number != m_query_sequence_number)
                return;

            if (network_error.has_value()) {
                warnln("Unable to fetch autocomplete suggestions: {}", Requests::network_error_to_string(*network_error));
                return;
            }
            if (response_code.has_value() && *response_code >= 400) {
                warnln("Received error response code {} from autocomplete engine: {}", *response_code, reason_phrase);
                return;
            }

            auto content_type = response_headers.get("Content-Type"sv);
            auto response_result = received_autocomplete_response(engine, content_type, payload);
            if (response_result.is_error()) {
                warnln("Unable to handle autocomplete response: {}", response_result.error());
                return;
            }

            auto merged_suggestions = merge_suggestions(
                query,
                prefer_navigational,
                max_results,
                move(local_suggestions),
                response_result.release_value());
            invoke_suggestions_query_complete(move(merged_suggestions));
        });
}

void Autocomplete::query_autocomplete_engine(String query)
{
    SuggestionOptions options;
    options.remote_enabled = Application::settings().autocomplete_remote_enabled();
    query_suggestions(move(query), options);
}

void Autocomplete::notify_omnibox_interaction()
{
    LocalSuggestionIndex::the().notify_omnibox_interaction();
}

StringView Autocomplete::local_index_rebuild_placeholder_text()
{
    return local_index_rebuild_placeholder;
}

void Autocomplete::record_committed_input(String const& text)
{
    auto trimmed_text = text.bytes_as_string_view().trim_whitespace();
    if (trimmed_text.is_empty())
        return;

    auto kind = looks_like_navigational(trimmed_text) ? SuggestionKind::Navigational : SuggestionKind::QueryCompletion;
    if (kind == SuggestionKind::Navigational)
        return;
    LocalSuggestionIndex::the().record(MUST(String::from_utf8(trimmed_text)), SuggestionSource::History, kind);
}

void Autocomplete::record_navigation(String const& text, Optional<String> title)
{
    auto trimmed_text = text.bytes_as_string_view().trim_whitespace();
    if (trimmed_text.is_empty())
        return;

    LocalSuggestionIndex::the().record(
        MUST(String::from_utf8(trimmed_text)),
        SuggestionSource::History,
        SuggestionKind::Navigational,
        move(title));
}

void Autocomplete::update_navigation_title(String const& text, String const& title)
{
    auto trimmed_text = text.bytes_as_string_view().trim_whitespace();
    if (trimmed_text.is_empty())
        return;

    LocalSuggestionIndex::the().update_navigation_title(
        MUST(String::from_utf8(trimmed_text)),
        title);
}

void Autocomplete::record_bookmark(String const& text)
{
    auto trimmed_text = text.bytes_as_string_view().trim_whitespace();
    if (trimmed_text.is_empty())
        return;

    LocalSuggestionIndex::the().record(
        MUST(String::from_utf8(trimmed_text)),
        SuggestionSource::Bookmark,
        SuggestionKind::Navigational);
}

void Autocomplete::rebuild_local_index_from_sources(LocalSuggestionSources sources)
{
    LocalSuggestionIndex::the().rebuild_from_sources(move(sources));
}

void Autocomplete::rebuild_local_index_from_current_entries()
{
    LocalSuggestionIndex::the().rebuild_indexes_from_current_entries();
}

void Autocomplete::schedule_local_index_rebuild_after_source_removal()
{
    LocalSuggestionIndex::the().schedule_rebuild_after_source_removal();
}

void Autocomplete::schedule_local_index_rebuild_after_source_removal(LocalSuggestionSources sources)
{
    LocalSuggestionIndex::the().schedule_rebuild_after_source_removal(move(sources));
}

void Autocomplete::clear_local_index()
{
    LocalSuggestionIndex::the().clear();
}

void Autocomplete::flush_local_index_to_disk(Core::EventLoop& event_loop)
{
    LocalSuggestionIndex::the().flush_to_disk(event_loop);
}

LocalSuggestionIndexStats Autocomplete::local_index_stats()
{
    return LocalSuggestionIndex::the().stats();
}

LocalSuggestionSources Autocomplete::local_index_sources_after_history_deletion(i64 delete_history_since_unix_seconds)
{
    return LocalSuggestionIndex::the().sources_after_history_deletion(delete_history_since_unix_seconds);
}

void Autocomplete::refresh_last_query_after_local_index_state_change()
{
    if (!m_has_active_query || !m_showing_local_index_rebuild_placeholder)
        return;

    query_suggestions(m_query, m_last_query_options);
}

void Autocomplete::notify_instances_about_local_index_state_change()
{
    auto* autocomplete = s_first_live_autocomplete_instance;
    while (autocomplete) {
        auto* next = autocomplete->m_next_live_instance;
        autocomplete->refresh_last_query_after_local_index_state_change();
        autocomplete = next;
    }
}

static ErrorOr<Vector<String>> parse_duckduckgo_autocomplete(JsonValue const& json)
{
    if (!json.is_array())
        return Error::from_string_literal("Expected DuckDuckGo autocomplete response to be a JSON array");

    Vector<String> results;
    results.ensure_capacity(json.as_array().size());

    TRY(json.as_array().try_for_each([&](JsonValue const& suggestion) -> ErrorOr<void> {
        if (!suggestion.is_object())
            return Error::from_string_literal("Invalid DuckDuckGo autocomplete response, expected value to be an object");

        if (auto value = suggestion.as_object().get_string("phrase"sv); value.has_value())
            results.unchecked_append(*value);

        return {};
    }));

    return results;
}

static ErrorOr<Vector<String>> parse_google_autocomplete(JsonValue const& json)
{
    if (!json.is_array())
        return Error::from_string_literal("Expected Google autocomplete response to be a JSON array");

    auto const& values = json.as_array();

    if (values.size() != 5)
        return Error::from_string_literal("Invalid Google autocomplete response, expected 5 elements in array");
    if (!values[1].is_array())
        return Error::from_string_literal("Invalid Google autocomplete response, expected second element to be an array");

    auto const& suggestions = values[1].as_array();

    Vector<String> results;
    results.ensure_capacity(suggestions.size());

    TRY(suggestions.try_for_each([&](JsonValue const& suggestion) -> ErrorOr<void> {
        if (!suggestion.is_string())
            return Error::from_string_literal("Invalid Google autocomplete response, expected value to be a string");

        results.unchecked_append(suggestion.as_string());
        return {};
    }));

    return results;
}

static ErrorOr<Vector<String>> parse_yahoo_autocomplete(JsonValue const& json)
{
    if (!json.is_object())
        return Error::from_string_literal("Expected Yahoo autocomplete response to be a JSON array");

    auto suggestions = json.as_object().get_array("r"sv);
    if (!suggestions.has_value())
        return Error::from_string_literal("Invalid Yahoo autocomplete response, expected \"r\" to be an object");

    Vector<String> results;
    results.ensure_capacity(suggestions->size());

    TRY(suggestions->try_for_each([&](JsonValue const& suggestion) -> ErrorOr<void> {
        if (!suggestion.is_object())
            return Error::from_string_literal("Invalid Yahoo autocomplete response, expected value to be an object");

        auto result = suggestion.as_object().get_string("k"sv);
        if (!result.has_value())
            return Error::from_string_literal("Invalid Yahoo autocomplete response, expected \"k\" to be a string");

        results.unchecked_append(*result);
        return {};
    }));

    return results;
}

ErrorOr<Vector<String>> Autocomplete::received_autocomplete_response(AutocompleteEngine const& engine, Optional<ByteString const&> content_type, StringView response)
{
    auto decoder = [&]() -> Optional<TextCodec::Decoder&> {
        if (!content_type.has_value())
            return {};

        auto mime_type = Web::MimeSniff::MimeType::parse(*content_type);
        if (!mime_type.has_value())
            return {};

        auto charset = mime_type->parameters().get("charset"sv);
        if (!charset.has_value())
            return {};

        return TextCodec::decoder_for_exact_name(*charset);
    }();

    if (!decoder.has_value())
        decoder = TextCodec::decoder_for_exact_name("UTF-8"sv);

    auto decoded_response = TRY(decoder->to_utf8(response));
    auto json = TRY(JsonValue::from_string(decoded_response));

    if (engine.name == "DuckDuckGo")
        return parse_duckduckgo_autocomplete(json);
    if (engine.name == "Google")
        return parse_google_autocomplete(json);
    if (engine.name == "Yahoo")
        return parse_yahoo_autocomplete(json);

    return Error::from_string_literal("Invalid engine name");
}

Vector<AutocompleteSuggestion> Autocomplete::merge_suggestions(
    StringView query,
    bool prefer_navigational,
    size_t max_results,
    Vector<AutocompleteSuggestion> local,
    Vector<String> remote)
{
    if (max_results == 0)
        return {};

    auto normalized_query = normalize_suggestion_text(query);
    Vector<AutocompleteSuggestion> merged_suggestions;
    merged_suggestions.ensure_capacity(max_results);
    HashMap<String, size_t> merged_indices;

    auto add_local_suggestion = [&](AutocompleteSuggestion suggestion) {
        if (merged_suggestions.size() >= max_results)
            return;

        auto key = dedup_key_for_suggestion_text(suggestion.text);
        if (key.is_empty())
            return;

        if (merged_indices.contains(key))
            return;

        merged_indices.set(move(key), merged_suggestions.size());
        merged_suggestions.unchecked_append(move(suggestion));
    };

    auto add_remote_suggestion = [&](AutocompleteSuggestion suggestion) {
        if (merged_suggestions.size() >= max_results)
            return;

        auto key = dedup_key_for_suggestion_text(suggestion.text);
        if (key.is_empty())
            return;

        if (auto existing_index = merged_indices.get(key); existing_index.has_value()) {
            auto& existing = merged_suggestions[*existing_index];
            if (existing.source == SuggestionSource::Remote && suggestion.score > existing.score)
                existing = move(suggestion);
            return;
        }

        merged_indices.set(move(key), merged_suggestions.size());
        merged_suggestions.unchecked_append(move(suggestion));
    };

    // Keep local suggestions stable and visible; append remote results into remaining slots.
    for (auto& suggestion : local)
        add_local_suggestion(move(suggestion));

    for (size_t index = 0; index < remote.size(); ++index) {
        if (merged_suggestions.size() >= max_results)
            break;

        auto text = normalize_remote_suggestion_for_display(remote[index]);
        if (text.is_empty())
            continue;

        auto normalized_text = normalize_suggestion_text(text);
        if (normalized_text.is_empty())
            continue;

        auto kind = looks_like_navigational(text) ? SuggestionKind::Navigational : SuggestionKind::QueryCompletion;

        auto score = 2.0 - static_cast<double>(index) * 0.1;
        if (normalized_text.starts_with_bytes(normalized_query.bytes_as_string_view()))
            score += 1.0;
        if (prefer_navigational && kind == SuggestionKind::Navigational)
            score += 0.5;
        else if (prefer_navigational)
            score -= 0.5;

        add_remote_suggestion(AutocompleteSuggestion {
            .text = move(text),
            .title = {},
            .kind = kind,
            .source = SuggestionSource::Remote,
            .score = score,
        });
    }

    return merged_suggestions;
}

void Autocomplete::invoke_suggestions_query_complete(Vector<AutocompleteSuggestion> suggestions) const
{
    if (on_suggestions_query_complete)
        on_suggestions_query_complete(suggestions);

    if (on_autocomplete_query_complete) {
        Vector<String> text_suggestions;
        text_suggestions.ensure_capacity(suggestions.size());

        for (auto const& suggestion : suggestions)
            text_suggestions.unchecked_append(suggestion.text);

        on_autocomplete_query_complete(move(text_suggestions));
    }
}

}
