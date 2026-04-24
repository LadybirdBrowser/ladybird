/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/Parser.h>
#include <LibURL/PublicSuffixData.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/URL.h>

namespace WebView {

Optional<URL::URL> sanitize_url(StringView location, Optional<SearchEngine> const& search_engine, AppendTLD append_tld)
{
    auto search_url_or_error = [&]() -> Optional<URL::URL> {
        if (!search_engine.has_value())
            return {};

        return URL::Parser::basic_parse(search_engine->format_search_query_for_navigation(location));
    };

    location = location.trim_whitespace();

    if (FileSystem::exists(location)) {
        auto path = FileSystem::real_path(location);
        if (!path.is_error())
            return URL::create_with_file_scheme(path.value());
        return search_url_or_error();
    }

    bool https_scheme_was_guessed = false;

    auto url = URL::create_with_url_or_path(location);

    if (!url.has_value() || url->scheme() == "localhost"sv) {
        url = URL::create_with_url_or_path(ByteString::formatted("https://{}", location));
        if (!url.has_value())
            return search_url_or_error();

        https_scheme_was_guessed = true;
    }

    // FIXME: Add support for other schemes, e.g. "mailto:". Firefox and Chrome open mailto: locations.
    static constexpr Array SUPPORTED_SCHEMES { "about"sv, "data"sv, "file"sv, "http"sv, "https"sv, "resource"sv };
    if (!any_of(SUPPORTED_SCHEMES, [&](StringView const& scheme) { return scheme == url->scheme(); }))
        return search_url_or_error();

    if (auto const& host = url->host(); host.has_value() && host->is_domain()) {
        auto const& domain = host->get<String>();

        if (domain.contains('"'))
            return search_url_or_error();

        // https://datatracker.ietf.org/doc/html/rfc2606
        static constexpr Array RESERVED_TLDS { ".test"sv, ".example"sv, ".invalid"sv, ".localhost"sv };
        if (any_of(RESERVED_TLDS, [&](StringView const& tld) { return domain.byte_count() > tld.length() && domain.ends_with_bytes(tld); }))
            return url;

        auto public_suffix = URL::PublicSuffixData::the()->get_public_suffix(domain);
        if (!public_suffix.has_value() || *public_suffix == domain) {
            if (append_tld == AppendTLD::Yes)
                url->set_host(MUST(String::formatted("{}.com", domain)));
            else if (https_scheme_was_guessed && domain != "localhost"sv)
                return search_url_or_error();
        }
    }

    return url;
}

bool location_looks_like_url(StringView location, AppendTLD append_tld)
{
    return sanitize_url(location, {}, append_tld).has_value();
}

static String normalized_web_url_for_autocomplete_comparison(URL::URL const& url)
{
    VERIFY(url.scheme().is_one_of("http"sv, "https"sv));

    // Address bar suggestions intentionally treat `http` and `https` variants
    // of the same web location as equivalent. Normalize away the scheme,
    // leading `www.`, default root slash, and default port so comparisons
    // match what the user actually typed.
    StringBuilder builder;

    if (!url.username().is_empty() || !url.password().is_empty()) {
        builder.append(url.username());
        if (!url.password().is_empty()) {
            builder.append(':');
            builder.append(url.password());
        }
        builder.append('@');
    }

    auto host = url.serialized_host();
    auto host_view = host.bytes_as_string_view();
    if (host_view.starts_with("www."sv, CaseSensitivity::CaseInsensitive))
        host_view = host_view.substring_view(4);
    builder.append(host_view);

    auto default_port = URL::default_port_for_scheme(url.scheme());
    if (url.port().has_value() && (!default_port.has_value() || *url.port() != *default_port))
        builder.appendff(":{}", *url.port());

    auto path = url.serialize_path();
    if (path != "/"sv)
        builder.append(path);

    if (url.query().has_value()) {
        builder.append('?');
        builder.append(*url.query());
    }

    return MUST(builder.to_string());
}

static String normalized_url_for_autocomplete_prefix_matching(URL::URL const& url)
{
    if (url.scheme().is_one_of("http"sv, "https"sv))
        return normalized_web_url_for_autocomplete_comparison(url);

    return url.serialize(URL::ExcludeFragment::Yes);
}

bool autocomplete_urls_match(StringView left, StringView right)
{
    auto left_url = sanitize_url(left);
    auto right_url = sanitize_url(right);
    if (!left_url.has_value() || !right_url.has_value())
        return false;

    if (left_url->scheme().is_one_of("http"sv, "https"sv)
        && right_url->scheme().is_one_of("http"sv, "https"sv))
        return normalized_web_url_for_autocomplete_comparison(*left_url) == normalized_web_url_for_autocomplete_comparison(*right_url);

    return left_url->equals(*right_url, URL::ExcludeFragment::Yes);
}

bool autocomplete_url_can_complete(StringView query, StringView suggestion)
{
    auto query_url = sanitize_url(query);
    auto suggestion_url = sanitize_url(suggestion);
    if (!query_url.has_value() || !suggestion_url.has_value())
        return false;

    auto normalized_query = normalized_url_for_autocomplete_prefix_matching(*query_url);
    auto normalized_suggestion = normalized_url_for_autocomplete_prefix_matching(*suggestion_url);

    if (normalized_suggestion.bytes_as_string_view().length() <= normalized_query.bytes_as_string_view().length())
        return false;

    return normalized_suggestion.starts_with_bytes(normalized_query, CaseSensitivity::CaseInsensitive);
}

Vector<URL::URL> sanitize_urls(ReadonlySpan<ByteString> raw_urls)
{
    Vector<URL::URL> sanitized_urls;
    sanitized_urls.ensure_capacity(raw_urls.size());

    for (auto const& raw_url : raw_urls) {
        if (auto url = sanitize_url(raw_url); url.has_value())
            sanitized_urls.unchecked_append(url.release_value());
    }

    if (sanitized_urls.is_empty())
        sanitized_urls.append(Application::settings().new_tab_page_url());

    return sanitized_urls;
}

static URLParts break_internal_url_into_parts(URL::URL const& url, StringView url_string)
{
    auto scheme = url_string.substring_view(0, url.scheme().bytes_as_string_view().length() + ":"sv.length());
    auto path = url_string.substring_view(scheme.length());

    return URLParts { scheme, path, {} };
}

static URLParts break_file_url_into_parts(URL::URL const& url, StringView url_string)
{
    auto scheme = url_string.substring_view(0, url.scheme().bytes_as_string_view().length() + "://"sv.length());
    auto path = url_string.substring_view(scheme.length());

    return URLParts { scheme, path, {} };
}

static URLParts break_web_url_into_parts(URL::URL const& url, StringView url_string)
{
    auto scheme = url_string.substring_view(0, url.scheme().bytes_as_string_view().length() + "://"sv.length());
    auto url_without_scheme = url_string.substring_view(scheme.length());

    StringView domain;
    StringView remainder;

    if (auto index = url_without_scheme.find_any_of("/?#"sv); index.has_value()) {
        domain = url_without_scheme.substring_view(0, *index);
        remainder = url_without_scheme.substring_view(*index);
    } else {
        domain = url_without_scheme;
    }

    auto public_suffix = URL::PublicSuffixData::the()->get_public_suffix(domain);
    if (!public_suffix.has_value() || !domain.ends_with(*public_suffix))
        return { scheme, domain, remainder };

    auto subdomain = domain.substring_view(0, domain.length() - public_suffix->bytes_as_string_view().length());
    subdomain = subdomain.trim("."sv, TrimMode::Right);

    if (auto index = subdomain.find_last('.'); index.has_value()) {
        subdomain = subdomain.substring_view(0, *index + 1);
        domain = domain.substring_view(subdomain.length());
    } else {
        subdomain = {};
    }

    auto scheme_and_subdomain = url_string.substring_view(0, scheme.length() + subdomain.length());
    return { scheme_and_subdomain, domain, remainder };
}

Optional<URLParts> break_url_into_parts(StringView url_string)
{
    auto maybe_url = URL::create_with_url_or_path(url_string);
    if (!maybe_url.has_value())
        return {};
    auto const& url = maybe_url.value();

    auto const& scheme = url.scheme();
    auto scheme_length = scheme.bytes_as_string_view().length();

    if (!url_string.starts_with(scheme))
        return {};

    auto schemeless_url = url_string.substring_view(scheme_length);

    if (schemeless_url.starts_with("://"sv)) {
        if (url.scheme() == "file"sv)
            return break_file_url_into_parts(url, url_string);
        if (url.scheme().is_one_of("http"sv, "https"sv))
            return break_web_url_into_parts(url, url_string);
    } else if (schemeless_url.starts_with(':')) {
        if (url.scheme().is_one_of("about"sv, "data"sv))
            return break_internal_url_into_parts(url, url_string);
    }

    return {};
}

URLType url_type(URL::URL const& url)
{
    if (url.scheme() == "mailto"sv)
        return URLType::Email;
    if (url.scheme() == "tel"sv)
        return URLType::Telephone;
    return URLType::Other;
}

ByteString url_text_to_copy(URL::URL const& url)
{
    auto url_text = url.to_byte_string();

    if (url.scheme() == "mailto"sv)
        return url_text.substring("mailto:"sv.length());
    if (url.scheme() == "tel"sv)
        return url_text.substring("tel:"sv.length());
    return url_text;
}

}
