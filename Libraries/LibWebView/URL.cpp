/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/CharacterTypes.h>
#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/Parser.h>
#include <LibURL/PublicSuffixData.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/URL.h>

namespace WebView {

// AD-HOC: When guessing the scheme for schemeless input, we prefer "https" — except for hosts that can't generally
//         obtain publicly-trusted TLS certs, and whose traffic never leaves the machine: loopback addresses and
//         localhost names (which the Secure Contexts spec treats as "potentially trustworthy", even over plain http),
//         as well as the unspecified addresses 0.0.0.0 and [::] (which reach loopback in practice), and private-use and
//         link-local addresses. Gecko and Blink likewise exempt all of those hosts from their HTTPS-upgrade machinery.
static bool should_guess_http_scheme_for_host(URL::Host const& host)
{
    if (host.is_loopback_or_localhost())
        return true;

    // NOTE: For hosts produced by the URL parser, to_u32() is the URL spec's IPv4 number — with the first octet in the
    //       most-significant byte.
    if (host.has<IPv4Address>()) {
        u32 const value = host.get<IPv4Address>().to_u32();
        return (value >> 24) == 0x00    // 0.0.0.0/8: "this network" (RFC 791).
            || (value >> 24) == 0x0a    // 10.0.0.0/8: private use (RFC 1918).
            || (value >> 20) == 0xac1   // 172.16.0.0/12: private use (RFC 1918).
            || (value >> 16) == 0xc0a8  // 192.168.0.0/16: private use (RFC 1918).
            || (value >> 16) == 0xa9fe; // 169.254.0.0/16: link-local (RFC 3927).
    }

    if (host.has<IPv6Address>()) {
        auto const& address = host.get<IPv6Address>();
        return address.is_zero()                // [::]: the unspecified address.
            || (address[0] & 0xfe00) == 0xfc00  // fc00::/7: unique local (RFC 4193).
            || (address[0] & 0xffc0) == 0xfe80; // fe80::/10: link-local (RFC 4291).
    }

    return false;
}

// FIXME: Add support for other schemes, e.g. "mailto:". Firefox and Chrome open mailto: locations.
static constexpr Array SUPPORTED_SCHEMES { "about"sv, "data"sv, "file"sv, "http"sv, "https"sv, "resource"sv };

// Schemes that sanitize_url() recognizes as schemes even though navigating to them is unsupported (input with one of
// these schemes becomes a search). Recognizing them keeps is_likely_host_with_port() below from reinterpreting input
// like "tel:911" or "javascript:0" as a hostname followed by a port number.
static constexpr Array RECOGNIZED_UNSUPPORTED_SCHEMES { "ftp"sv, "javascript"sv, "mailto"sv, "tel"sv, "ws"sv, "wss"sv };

static bool is_recognized_scheme(StringView scheme)
{
    return any_of(SUPPORTED_SCHEMES, [&](StringView supported_scheme) { return supported_scheme == scheme; })
        || any_of(RECOGNIZED_UNSUPPORTED_SCHEMES, [&](StringView unsupported_scheme) { return unsupported_scheme == scheme; });
}

// AD-HOC: A dotted hostname followed by a port (e.g. "example.org:8000") is also a syntactically valid URL scheme — so
//         the URL parser accepts it as one, and schemeless host-with-port input would then fall through to a search.
//         So, when the parsed scheme isn’t a scheme we know — and everything between the first colon and its first '/',
//         '?', or '#' (or its end) is one or more ASCII digits — treat it as a host with a port instead. Chromium's
//         url_fixer and Firefox's URIFixup likewise decline to treat an unknown scheme as a scheme when a port-like
//         number follows the colon. Port range checking is left to the URL parser: For "example.org:99999", etc., the
//         "https://" retry below fails to parse, and the input becomes a search — just as in Chromium and Firefox.
static bool is_likely_host_with_port(URL::URL const& url, StringView location)
{
    if (is_recognized_scheme(url.scheme()))
        return false;

    auto first_colon_index = location.find(':');
    if (!first_colon_index.has_value())
        return false;

    auto after_colon = location.substring_view(*first_colon_index + 1);
    auto port_length = after_colon.find_any_of("/?#"sv).value_or(after_colon.length());
    auto port = after_colon.substring_view(0, port_length);
    return !port.is_empty() && all_of(port, is_ascii_digit);
}

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
    ByteString schemeless_location;

    auto url = URL::create_with_url_or_path(location);

    if (!url.has_value() || is_likely_host_with_port(*url, location)) {
        schemeless_location = location;

        // AD-HOC: A bare IPv6 address can't be a URL host until it's wrapped in square brackets. Wrap it here, so that
        //         e.g., "::1" becomes a navigation — instead of falling through to a search. Chromium's omnibox
        //         likewise classifies bare IPv6 addresses as navigations.
        if (auto ipv6_address = IPv6Address::from_string(location); ipv6_address.has_value())
            schemeless_location = ByteString::formatted("[{}]", MUST(ipv6_address->to_string()));

        url = URL::create_with_url_or_path(ByteString::formatted("https://{}", schemeless_location));
        if (!url.has_value())
            return search_url_or_error();

        https_scheme_was_guessed = true;
    }

    if (!any_of(SUPPORTED_SCHEMES, [&](StringView const& scheme) { return scheme == url->scheme(); }))
        return search_url_or_error();

    if (auto const& host = url->host(); host.has_value() && host->is_domain()) {
        auto const& domain = host->get<String>();

        if (domain.contains('"'))
            return search_url_or_error();

        // https://datatracker.ietf.org/doc/html/rfc2606
        static constexpr Array RESERVED_TLDS { ".test"sv, ".example"sv, ".invalid"sv, ".localhost"sv };
        bool has_reserved_tld = any_of(RESERVED_TLDS, [&](StringView const& tld) { return domain.byte_count() > tld.length() && domain.ends_with_bytes(tld); });

        if (!has_reserved_tld) {
            auto public_suffix = URL::PublicSuffixData::find_matching_public_suffix(domain, URL::PublicSuffixData::IncludeStarRule::No);
            if (!public_suffix.has_value() || *public_suffix == domain) {
                if (append_tld == AppendTLD::Yes)
                    url->set_host(MUST(String::formatted("{}.com", domain)));
                else if (https_scheme_was_guessed && !host->is_loopback_or_localhost())
                    return search_url_or_error();
            }
        }
    }

    // AD-HOC: We guessed "https" above without knowing the host. Now that the host is known, guess "http" instead where
    //         https can't reasonably work. Reparsing the input keeps intact a port that's default for one scheme but
    //         not the other. An AppendTLD::Yes rewrite above never produces a host for which this branch is taken — so,
    //         the rewrite can't be lost by reparsing.
    if (https_scheme_was_guessed && url->host().has_value() && should_guess_http_scheme_for_host(*url->host()))
        url = URL::create_with_url_or_path(ByteString::formatted("http://{}", schemeless_location));

    return url;
}

bool location_looks_like_url(StringView location, AppendTLD append_tld)
{
    return sanitize_url(location, {}, append_tld).has_value();
}

Optional<URL::URL> url_from_text(StringView text)
{
    auto trimmed_text = text.trim_whitespace();
    if (trimmed_text.is_empty() || trimmed_text.is_whitespace())
        return {};

    if (trimmed_text.starts_with('.') || trimmed_text.starts_with('/'))
        return {};

    return sanitize_url(trimmed_text);
}

bool autocomplete_urls_match(StringView left, StringView right)
{
    auto parse_destination = [](StringView value) {
        auto url = URL::Parser::basic_parse(value);
        if (url.has_value() && url->scheme().is_one_of("about"sv, "data"sv, "file"sv, "http"sv, "https"sv, "resource"sv))
            return url;
        return URL::Parser::basic_parse(MUST(String::formatted("https://{}", value)));
    };

    auto left_url = parse_destination(left);
    auto right_url = parse_destination(right);
    if (!left_url.has_value() || !right_url.has_value())
        return false;

    return left_url->equals(*right_url, URL::ExcludeFragment::Yes);
}

bool autocomplete_url_can_complete(StringView query, StringView suggestion)
{
    auto suggestion_url = URL::Parser::basic_parse(suggestion);
    if (query.is_empty() || !suggestion_url.has_value())
        return false;

    auto can_complete_form = [&](StringView form) {
        if (form.ends_with('/'))
            form = form.substring_view(0, form.length() - 1);
        return form.length() > query.length()
            && form.starts_with(query, CaseSensitivity::CaseInsensitive);
    };

    auto serialized_suggestion = suggestion_url->serialize(URL::ExcludeFragment::Yes);
    if (can_complete_form(serialized_suggestion))
        return true;

    if (!suggestion_url->scheme().is_one_of("http"sv, "https"sv))
        return false;

    auto serialized_suggestion_view = serialized_suggestion.bytes_as_string_view();
    auto scheme_end = serialized_suggestion_view.find("://"sv);
    VERIFY(scheme_end.has_value());
    auto without_scheme = serialized_suggestion_view.substring_view(*scheme_end + 3);
    if (can_complete_form(without_scheme))
        return true;

    if (without_scheme.starts_with("www."sv, CaseSensitivity::CaseInsensitive)) {
        if (can_complete_form(without_scheme.substring_view(4)))
            return true;

        StringBuilder without_www;
        without_www.append(serialized_suggestion_view.substring_view(0, *scheme_end + 3));
        without_www.append(without_scheme.substring_view(4));
        if (can_complete_form(without_www.string_view()))
            return true;
    }

    return false;
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

String url_for_display(URL::URL const& url)
{
    if (!url.scheme().is_one_of("http"sv, "https"sv))
        return url.serialize();

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

    if (url.port().has_value())
        builder.appendff(":{}", *url.port());

    auto path = url.serialize_path();
    if (path != "/"sv || url.query().has_value() || url.fragment().has_value())
        builder.append(path);

    if (url.query().has_value()) {
        builder.append('?');
        builder.append(*url.query());
    }

    if (url.fragment().has_value()) {
        builder.append('#');
        builder.append(*url.fragment());
    }

    return MUST(builder.to_string());
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

    auto public_suffix = URL::PublicSuffixData::find_matching_public_suffix(domain, URL::PublicSuffixData::IncludeStarRule::No);
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
