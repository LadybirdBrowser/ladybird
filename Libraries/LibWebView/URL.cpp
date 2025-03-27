/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/Parser.h>
#include <LibWebView/URL.h>

namespace WebView {

Optional<URL::URL> sanitize_url(StringView location, Optional<StringView> search_engine, AppendTLD append_tld)
{
    auto search_url_or_error = [&]() -> Optional<URL::URL> {
        if (!search_engine.has_value())
            return {};

        return URL::Parser::basic_parse(MUST(String::formatted(*search_engine, URL::percent_encode(location))));
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

    if (!url.is_valid()) {
        url = URL::create_with_url_or_path(ByteString::formatted("https://{}", location));

        if (!url.is_valid())
            return search_url_or_error();

        https_scheme_was_guessed = true;
    }

    static constexpr Array SUPPORTED_SCHEMES { "about"sv, "data"sv, "file"sv, "http"sv, "https"sv, "resource"sv };
    if (!any_of(SUPPORTED_SCHEMES, [&](StringView const& scheme) { return scheme == url.scheme(); }))
        return search_url_or_error();
    // FIXME: Add support for other schemes, e.g. "mailto:". Firefox and Chrome open mailto: locations.

    auto const& host = url.host();
    if (host.has_value() && host->is_domain()) {
        auto const& domain = host->get<String>();

        if (domain.contains('"'))
            return search_url_or_error();

        // https://datatracker.ietf.org/doc/html/rfc2606
        static constexpr Array RESERVED_TLDS { ".test"sv, ".example"sv, ".invalid"sv, ".localhost"sv };
        if (any_of(RESERVED_TLDS, [&](StringView const& tld) { return domain.byte_count() > tld.length() && domain.ends_with_bytes(tld); }))
            return url;

        auto public_suffix = URL::get_public_suffix(domain);
        if (!public_suffix.has_value() || *public_suffix == domain) {
            if (append_tld == AppendTLD::Yes)
                url.set_host(MUST(String::formatted("{}.com", domain)));
            else if (https_scheme_was_guessed && domain != "localhost"sv)
                return search_url_or_error();
        }
    }

    return url;
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
