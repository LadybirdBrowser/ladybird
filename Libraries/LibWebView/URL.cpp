/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/Parser.h>
#include <LibWebView/URL.h>

namespace WebView {

Optional<URL::URL> sanitize_url(StringView location, Optional<StringView> search_engine, AppendTLD append_tld)
{
    StringView const location_trimmed = location.trim_whitespace();

    // FIXME: On empty return, the user should be presented with Requests::NetworkError::MalformedUrl
    //        or similar, maybe without issuing the request.
    //        Firefox has "Hmm that address doesn't look right." on "about:error".
    // Current behaviour: the browser simply reverts back to the latest good state
    // (about:newtab or last displayed site), overwriting the location entered by the user,
    // without any feedback. Not cool.

    auto search_url_or_error = [&]() -> Optional<URL::URL> {
        if (!search_engine.has_value())
            return {};

        return URL::Parser::basic_parse(MUST(String::formatted(*search_engine, URL::percent_encode(location_trimmed))));
    };

    auto url_or_error = [&]() -> Optional<URL::URL> {
        auto url = URL::create_with_url_or_path(location_trimmed);
        if (!url.is_valid())
            return {};

        return url;
    };

    if (FileSystem::exists(location_trimmed)) {
        auto path = FileSystem::real_path(location_trimmed);
        if (path.is_error())
            return search_url_or_error();

        return url_or_error();
    }

    if (location_trimmed.contains('"'))
        return search_url_or_error();

    auto const index_for_first_space = location_trimmed.find(' ');
    if (index_for_first_space.has_value()) {
        auto const head = location_trimmed.substring_view(0, index_for_first_space.value());
        auto const tail = location_trimmed.substring_view(index_for_first_space.value() + 1);

        if (!head.contains("://"sv))
            return search_url_or_error();

        auto const head_as_url = URL::create_with_url_or_path(head);
        if (!head_as_url.is_valid() || !head_as_url.host().has_value())
            return search_url_or_error();

        if (head.ends_with('/') || (head_as_url.paths().size() > 0 && head_as_url.paths().at(0).byte_count() > 0))
            // "http://example.com/ once" or "http://example.com/some cool page"
            return URL::create_with_url_or_path(ByteString::formatted("{}%20{}"sv, head, URL::percent_encode(tail)));

        return search_url_or_error(); // "http://example.org and other examples"
    }

    if (location_trimmed.starts_with("about:"sv) || location_trimmed.contains("://"sv) || location_trimmed.starts_with("data:"sv))
        return url_or_error();

    auto location_with_scheme = URL::create_with_url_or_path(ByteString::formatted("https://{}"sv, location_trimmed));

    if (!location_with_scheme.is_valid())
        return search_url_or_error();

    auto const& host = location_with_scheme.host();
    if (!host.has_value())
        return search_url_or_error();

    auto host_as_string = host->serialize();
    auto public_suffix = URL::get_public_suffix(host_as_string);

    if (!public_suffix.has_value() || *public_suffix == host_as_string) { // "nosuffix" or "com"
        if (append_tld == AppendTLD::Yes) {
            location_with_scheme.set_host(MUST(String::formatted("{}.com", host_as_string)));
            return location_with_scheme;
        }

        if (host_as_string == "localhost"sv)
            return location_with_scheme;

        return search_url_or_error();
    }

    return location_with_scheme;
}

Vector<URL::URL> sanitize_urls(ReadonlySpan<ByteString> raw_urls, URL::URL const& new_tab_page_url)
{
    Vector<URL::URL> sanitized_urls;
    sanitized_urls.ensure_capacity(raw_urls.size());

    for (auto const& raw_url : raw_urls) {
        if (auto url = sanitize_url(raw_url); url.has_value())
            sanitized_urls.unchecked_append(url.release_value());
    }

    if (sanitized_urls.is_empty())
        sanitized_urls.append(new_tab_page_url);

    return sanitized_urls;
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

    auto public_suffix = URL::get_public_suffix(domain);
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
    auto url = URL::create_with_url_or_path(url_string);
    if (!url.is_valid())
        return {};

    auto const& scheme = url.scheme();
    auto scheme_length = scheme.bytes_as_string_view().length();

    if (!url_string.starts_with(scheme))
        return {};
    if (!url_string.substring_view(scheme_length).starts_with("://"sv))
        return {};

    if (url.scheme() == "file"sv)
        return break_file_url_into_parts(url, url_string);
    if (url.scheme().is_one_of("http"sv, "https"sv))
        return break_web_url_into_parts(url, url_string);

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

String url_text_to_copy(URL::URL const& url)
{
    auto url_text = url.to_string();

    if (url.scheme() == "mailto"sv)
        return MUST(url_text.substring_from_byte_offset("mailto:"sv.length()));

    if (url.scheme() == "tel"sv)
        return MUST(url_text.substring_from_byte_offset("tel:"sv.length()));

    return url_text;
}

}
