/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
#include <LibWebView/ExternalURLHandler.h>
#include <LibWebView/URL.h>

namespace WebView {

bool is_url_handled_internally(URL::URL const& url)
{
    return url.scheme().is_one_of("about"sv, "blob"sv, "data"sv, "file"sv, "http"sv, "https"sv, "resource"sv);
}

static bool has_valid_explicit_port(StringView input)
{
    auto authority_end = input.find_any_of("/?#"sv).value_or(input.length());
    auto authority = input.substring_view(0, authority_end);

    Optional<size_t> port_separator;
    if (authority.starts_with('[')) {
        auto closing_bracket = authority.find(']');
        if (!closing_bracket.has_value() || *closing_bracket + 1 >= authority.length() || authority[*closing_bracket + 1] != ':')
            return false;
        port_separator = *closing_bracket + 1;
    } else {
        port_separator = authority.find_last(':');
        if (!port_separator.has_value())
            return false;
    }

    auto port = authority.substring_view(*port_separator + 1);
    if (port.is_empty())
        return false;
    for (auto byte : port.bytes()) {
        if (!is_ascii_digit(byte))
            return false;
    }

    auto url = URL::Parser::basic_parse(MUST(String::formatted("https://{}", input)));
    return url.has_value() && url->scheme() == "https"sv;
}

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

ClassifiedUserInput classify_user_input(StringView location, AppendTLD append_tld)
{
    location = location.trim_whitespace();

    if (FileSystem::exists(location)) {
        auto path = FileSystem::real_path(location);
        if (!path.is_error())
            return { UserInputClassification::InternalURL, URL::create_with_file_scheme(path.value()) };
        return {};
    }

    bool https_scheme_was_guessed = false;
    bool input_has_explicit_port = false;
    ByteString schemeless_location;

    auto url = URL::create_with_url_or_path(location);
    // These known schemes do not use an authority, so recognize them before
    // testing whether the text could instead be a host and port.
    auto has_known_external_scheme = url.has_value() && url->scheme().is_one_of("geo"sv, "mailto"sv, "sms"sv, "tel"sv);

    if (url.has_value() && is_url_handled_internally(*url)) {
        // Continue below to apply the existing domain and public suffix checks.
    } else if (url.has_value() && ExternalURLRequestPolicy::is_denied_scheme(url->scheme())) {
        return {};
    } else if (has_known_external_scheme) {
        return { UserInputClassification::ExternalURL, move(url) };
    } else if (has_valid_explicit_port(location)) {
        schemeless_location = location;
        url = URL::create_with_url_or_path(ByteString::formatted("https://{}", location));
        if (!url.has_value())
            return {};

        https_scheme_was_guessed = true;
        input_has_explicit_port = true;
    } else if (url.has_value()) {
        return { UserInputClassification::ExternalURL, move(url) };
    } else {
        schemeless_location = location;

        // AD-HOC: A bare IPv6 address can't be a URL host until it's wrapped in square brackets. Wrap it here, so that
        //         e.g., "::1" becomes a navigation — instead of falling through to a search. Chromium's omnibox
        //         likewise classifies bare IPv6 addresses as navigations.
        if (auto ipv6_address = IPv6Address::from_string(location); ipv6_address.has_value())
            schemeless_location = ByteString::formatted("[{}]", MUST(ipv6_address->to_string()));

        url = URL::create_with_url_or_path(ByteString::formatted("https://{}", schemeless_location));
        if (!url.has_value())
            return {};

        https_scheme_was_guessed = true;
    }

    if (!is_url_handled_internally(*url))
        return {};

    if (auto const& host = url->host(); host.has_value() && host->is_domain()) {
        auto const& domain = host->get<String>();

        if (domain.contains('"'))
            return {};

        // https://datatracker.ietf.org/doc/html/rfc2606
        static constexpr Array RESERVED_TLDS { ".test"sv, ".example"sv, ".invalid"sv, ".localhost"sv };
        bool has_reserved_tld = any_of(RESERVED_TLDS, [&](StringView const& tld) { return domain.byte_count() > tld.length() && domain.ends_with_bytes(tld); });

        if (!has_reserved_tld) {
            auto public_suffix = URL::PublicSuffixData::find_matching_public_suffix(domain, URL::PublicSuffixData::IncludeStarRule::No);
            if (!public_suffix.has_value() || *public_suffix == domain) {
                if (append_tld == AppendTLD::Yes)
                    url->set_host(MUST(String::formatted("{}.com", domain)));
                else if (https_scheme_was_guessed && !host->is_loopback_or_localhost() && !input_has_explicit_port)
                    return {};
            }
        }
    }

    // AD-HOC: We guessed "https" above without knowing the host. Now that the host is known, guess "http" instead where
    //         https can't reasonably work. Reparsing the input keeps intact a port that's default for one scheme but
    //         not the other. An AppendTLD::Yes rewrite above never produces a host for which this branch is taken — so,
    //         the rewrite can't be lost by reparsing.
    if (https_scheme_was_guessed && url->host().has_value() && should_guess_http_scheme_for_host(*url->host()))
        url = URL::create_with_url_or_path(ByteString::formatted("http://{}", schemeless_location));

    return { UserInputClassification::InternalURL, move(url) };
}

Optional<URL::URL> sanitize_url(StringView location, Optional<SearchEngine> const& search_engine, AppendTLD append_tld)
{
    location = location.trim_whitespace();

    auto classified_input = classify_user_input(location, append_tld);
    if (classified_input.classification == UserInputClassification::InternalURL)
        return classified_input.url;

    if (!search_engine.has_value())
        return {};

    return URL::Parser::basic_parse(search_engine->format_search_query_for_navigation(location));
}

bool location_looks_like_url(StringView location, AppendTLD append_tld)
{
    switch (classify_user_input(location, append_tld).classification) {
    case UserInputClassification::InternalURL:
    case UserInputClassification::ExternalURL:
        return true;
    case UserInputClassification::Search:
        return false;
    }
    VERIFY_NOT_REACHED();
}

Optional<URL::URL> url_from_text(StringView text)
{
    auto trimmed_text = text.trim_whitespace();
    if (trimmed_text.is_empty() || trimmed_text.is_whitespace())
        return {};

    if (trimmed_text.starts_with('.') || trimmed_text.starts_with('/'))
        return {};

    // The URL parser accepts abbreviated IPv4 addresses, causing numeric text such as "88" to be parsed as
    // 0.0.0.88. For selected text, only recognize an IPv4 address when all four parts are present.
    auto hostname_end = trimmed_text.find_any_of(":/?#"sv).value_or(trimmed_text.length());
    auto hostname = trimmed_text.substring_view(0, hostname_end);
    auto is_numeric_hostname = all_of(hostname.bytes(), [](auto byte) { return is_ascii_digit(byte) || byte == '.'; });
    if (is_numeric_hostname) {
        auto parts = hostname.split_view('.', SplitBehavior::KeepEmpty);
        if (parts.size() != 4 || any_of(parts, [](auto part) { return part.is_empty(); }))
            return {};
    }

    return sanitize_url(trimmed_text);
}

bool autocomplete_urls_match(StringView left, StringView right)
{
    auto parse_destination = [](StringView value) {
        auto classified_input = classify_user_input(value);
        if (classified_input.url.has_value())
            return classified_input.url;
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
