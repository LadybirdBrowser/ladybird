/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/URL.h>

#if defined(ENABLE_PUBLIC_SUFFIX)
#    include <LibWebView/PublicSuffixData.h>
#endif

namespace WebView {

static Optional<URL::URL> query_public_suffix_list(StringView url_string)
{
    auto out = MUST(String::from_utf8(url_string));
    if (!out.starts_with_bytes("about:"sv) && !out.contains("://"sv))
        out = MUST(String::formatted("https://{}"sv, out));

    auto url = URL::create_with_url_or_path(out.to_byte_string());
    if (!url.is_valid())
        return {};

    if (url.host().has<URL::IPv4Address>() || url.host().has<URL::IPv6Address>())
        return url;

    if (url.scheme() != "http"sv && url.scheme() != "https"sv)
        return url;

    if (url.host().has<String>()) {
        auto const& host = url.host().get<String>();

        if (auto public_suffix = get_public_suffix(host); public_suffix.has_value())
            return url;

        if (host.ends_with_bytes(".local"sv) || host.ends_with_bytes("localhost"sv))
            return url;
    }

    return {};
}

bool is_public_suffix([[maybe_unused]] StringView host)
{
#if defined(ENABLE_PUBLIC_SUFFIX)
    return PublicSuffixData::the()->is_public_suffix(host);
#else
    return false;
#endif
}

Optional<String> get_public_suffix([[maybe_unused]] StringView host)
{
#if defined(ENABLE_PUBLIC_SUFFIX)
    return MUST(PublicSuffixData::the()->get_public_suffix(host));
#else
    return {};
#endif
}

Optional<URL::URL> sanitize_url(StringView url, Optional<StringView> search_engine, AppendTLD append_tld)
{
    if (FileSystem::exists(url)) {
        auto path = FileSystem::real_path(url);
        if (path.is_error())
            return {};

        return URL::create_with_file_scheme(path.value());
    }

    auto format_search_engine = [&]() -> Optional<URL::URL> {
        if (!search_engine.has_value())
            return {};

        return MUST(String::formatted(*search_engine, URL::percent_decode(url)));
    };

    String url_buffer;

    if (append_tld == AppendTLD::Yes) {
        // FIXME: Expand the list of top level domains.
        if (!url.ends_with(".com"sv) && !url.ends_with(".net"sv) && !url.ends_with(".org"sv)) {
            url_buffer = MUST(String::formatted("{}.com", url));
            url = url_buffer;
        }
    }

    auto result = query_public_suffix_list(url);
    if (!result.has_value())
        return format_search_engine();

    return result.release_value();
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

    auto public_suffix = get_public_suffix(domain);
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
    auto url_text = MUST(url.to_string());

    if (url.scheme() == "mailto"sv)
        return MUST(url_text.substring_from_byte_offset("mailto:"sv.length()));

    if (url.scheme() == "tel"sv)
        return MUST(url_text.substring_from_byte_offset("tel:"sv.length()));

    return url_text;
}

}
