/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/KeywordSources.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/Directives/SourceExpression.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::ContentSecurityPolicy::Directives {

// https://w3c.github.io/webappsec-csp/#directive-fallback-list
// Will return an ordered set of the fallback directives for a specific directive.
// The returned ordered set is sorted from most relevant to least relevant and it includes the effective directive
// itself.
static HashMap<StringView, Vector<StringView>> fetch_directive_fallback_list {
    // "script-src-elem"
    //      1. Return << "script-src-elem", "script-src", "default-src" >>.
    { "script-src-elem"sv, { "script-src-elem"sv, "script-src"sv, "default-src"sv } },

    // "script-src-attr"
    //      1. Return << "script-src-attr", "script-src", "default-src" >>.
    { "script-src-attr"sv, { "script-src-attr"sv, "script-src"sv, "default-src"sv } },

    // "style-src-elem"
    //      1. Return << "style-src-elem", "style-src", "default-src" >>.
    { "style-src-elem"sv, { "style-src-elem"sv, "style-src"sv, "default-src"sv } },

    // "style-src-attr"
    //      1. Return << "style-src-attr", "style-src", "default-src" >>.
    { "style-src-attr"sv, { "style-src-attr"sv, "style-src"sv, "default-src"sv } },

    // "worker-src"
    //      1. Return << "worker-src", "child-src", "script-src", "default-src" >>.
    { "worker-src"sv, { "worker-src"sv, "child-src"sv, "script-src"sv, "default-src"sv } },

    // "connect-src"
    //      1. Return << "connect-src", "default-src" >>.
    { "connect-src"sv, { "connect-src"sv, "default-src"sv } },

    // "manifest-src"
    //      1. Return << "manifest-src", "default-src" >>.
    { "manifest-src"sv, { "manifest-src"sv, "default-src"sv } },

    // "object-src"
    //      1. Return << "object-src", "default-src" >>.
    { "object-src"sv, { "object-src"sv, "default-src"sv } },

    // "frame-src"
    //      1. Return << "frame-src", "child-src", "default-src" >>.
    { "frame-src"sv, { "frame-src"sv, "child-src"sv, "default-src"sv } },

    // "media-src"
    //      1. Return << "media-src", "default-src" >>.
    { "media-src"sv, { "media-src"sv, "default-src"sv } },

    // "font-src"
    //      1. Return << "font-src", "default-src" >>.
    { "font-src"sv, { "font-src"sv, "default-src"sv } },

    // "img-src"
    //      1. Return << "img-src", "default-src" >>.
    { "img-src"sv, { "img-src"sv, "default-src"sv } },
};

// https://w3c.github.io/webappsec-csp/#effective-directive-for-a-request
Optional<FlyString> get_the_effective_directive_for_request(GC::Ref<Fetch::Infrastructure::Request const> request)
{
    // Each fetch directive controls a specific destination of request. Given a request request, the following algorithm
    // returns either null or the name of the request’s effective directive:
    // 1. If request’s initiator is "prefetch" or "prerender", return default-src.
    if (request->initiator() == Fetch::Infrastructure::Request::Initiator::Prefetch || request->initiator() == Fetch::Infrastructure::Request::Initiator::Prerender)
        return Names::DefaultSrc;

    // 2. Switch on request’s destination, and execute the associated steps:
    // the empty string
    //      1. Return connect-src.
    if (!request->destination().has_value())
        return Names::ConnectSrc;

    switch (request->destination().value()) {
    // "manifest"
    //      1. Return manifest-src.
    case Fetch::Infrastructure::Request::Destination::Manifest:
        return Names::ManifestSrc;
    // "object"
    // "embed"
    //      1. Return object-src.
    case Fetch::Infrastructure::Request::Destination::Object:
    case Fetch::Infrastructure::Request::Destination::Embed:
        return Names::ObjectSrc;
    // "frame"
    // "iframe"
    //      1. Return frame-src.
    case Fetch::Infrastructure::Request::Destination::Frame:
    case Fetch::Infrastructure::Request::Destination::IFrame:
        return Names::FrameSrc;
    // "audio"
    // "track"
    // "video"
    //      1. Return media-src.
    case Fetch::Infrastructure::Request::Destination::Audio:
    case Fetch::Infrastructure::Request::Destination::Track:
    case Fetch::Infrastructure::Request::Destination::Video:
        return Names::MediaSrc;
    // "font"
    //      1. Return font-src.
    case Fetch::Infrastructure::Request::Destination::Font:
        return Names::FontSrc;
    // "image"
    //      1. Return img-src.
    case Fetch::Infrastructure::Request::Destination::Image:
        return Names::ImgSrc;
    // "style"
    //      1. Return style-src-elem.
    case Fetch::Infrastructure::Request::Destination::Style:
        return Names::StyleSrcElem;
    // "script"
    // "xslt"
    // "audioworklet"
    // "paintworklet"
    //      1. Return script-src-elem.
    case Fetch::Infrastructure::Request::Destination::Script:
    case Fetch::Infrastructure::Request::Destination::XSLT:
    case Fetch::Infrastructure::Request::Destination::AudioWorklet:
    case Fetch::Infrastructure::Request::Destination::PaintWorklet:
        return Names::ScriptSrcElem;
    // "serviceworker"
    // "sharedworker"
    // "worker"
    //      1. Return worker-src.
    case Fetch::Infrastructure::Request::Destination::ServiceWorker:
    case Fetch::Infrastructure::Request::Destination::SharedWorker:
    case Fetch::Infrastructure::Request::Destination::Worker:
        return Names::WorkerSrc;
    // "json"
    // "webidentity"
    //      1. Return connect-src.
    case Fetch::Infrastructure::Request::Destination::JSON:
    case Fetch::Infrastructure::Request::Destination::WebIdentity:
        return Names::ConnectSrc;
    // "report"
    //      1. Return null.
    case Fetch::Infrastructure::Request::Destination::Report:
        return OptionalNone {};
    // 3. Return connect-src.
    // Spec Note: The algorithm returns connect-src as a default fallback. This is intended for new fetch destinations
    //            that are added and which don’t explicitly fall into one of the other categories.
    default:
        return Names::ConnectSrc;
    }
}

// https://w3c.github.io/webappsec-csp/#directive-fallback-list
Vector<StringView> get_fetch_directive_fallback_list(Optional<FlyString> directive_name)
{
    if (!directive_name.has_value())
        return {};

    auto list_iterator = fetch_directive_fallback_list.find(directive_name.value());
    if (list_iterator == fetch_directive_fallback_list.end())
        return {};

    return list_iterator->value;
}

// https://w3c.github.io/webappsec-csp/#should-directive-execute
ShouldExecute should_fetch_directive_execute(Optional<FlyString> effective_directive_name, FlyString const& directive_name, GC::Ref<Policy const> policy)
{
    // 1. Let directive fallback list be the result of executing § 6.8.3 Get fetch directive fallback list on effective
    //    directive name.
    auto const& directive_fallback_list = get_fetch_directive_fallback_list(effective_directive_name);

    // 2. For each fallback directive of directive fallback list:
    for (auto fallback_directive : directive_fallback_list) {
        // 1. If directive name is fallback directive, Return "Yes".
        if (directive_name == fallback_directive)
            return ShouldExecute::Yes;

        // 2. If policy contains a directive whose name is fallback directive, Return "No".
        if (policy->contains_directive_with_name(fallback_directive))
            return ShouldExecute::No;
    }

    // 3. Return "No".
    return ShouldExecute::No;
}

// https://w3c.github.io/webappsec-csp/#effective-directive-for-inline-check
FlyString get_the_effective_directive_for_inline_checks(Directive::InlineType type)
{
    // Spec Note: While the effective directive is only defined for requests, in this algorithm it is used similarly to
    //            mean the directive that is most relevant to a particular type of inline check.

    // Switch on type:
    switch (type) {
        // "script"
        // "navigation"
        //    Return script-src-elem.
    case Directive::InlineType::Script:
    case Directive::InlineType::Navigation:
        return Names::ScriptSrcElem;
        // "script attribute"
        //    Return script-src-attr.
    case Directive::InlineType::ScriptAttribute:
        return Names::ScriptSrcAttr;
        // "style"
        //    Return style-src-elem.
    case Directive::InlineType::Style:
        return Names::StyleSrcElem;
        // "style attribute"
        //    Return style-src-attr.
    case Directive::InlineType::StyleAttribute:
        return Names::StyleSrcAttr;
    }

    // 2. Return null.
    // FIXME: File spec issue that this should be invalid, as the result of this algorithm ends up being piped into
    //        Violation's effective directive, which is defined to be a non-empty string.
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/webappsec-csp/#scheme-part-match
// An ASCII string scheme-part matches another ASCII string if a CSP source expression that contained the first as a
// scheme-part could potentially match a URL containing the latter as a scheme. For example, we say that "http"
// scheme-part matches "https".
// More formally, two ASCII strings A and B are said to scheme-part match if the following algorithm returns "Matches":
// Spec Note: The matching relation is asymmetric. For example, the source expressions https: and https://example.com/
//            do not match the URL http://example.com/. We always allow a secure upgrade from an explicitly insecure
//            expression. script-src http: is treated as equivalent to script-src http: https:,
//            script-src http://example.com to script-src http://example.com https://example.com,
//            and connect-src ws: to connect-src ws: wss:.
[[nodiscard]] static MatchResult scheme_part_matches(StringView a, StringView b)
{
    // 1. If one of the following is true, return "Matches":
    //    1. A is an ASCII case-insensitive match for B.
    if (a.equals_ignoring_ascii_case(b))
        return MatchResult::Matches;

    //    2. A is an ASCII case-insensitive match for "http", and B is an ASCII case-insensitive match for "https".
    if (a.equals_ignoring_ascii_case("http"sv) && b.equals_ignoring_ascii_case("https"sv))
        return MatchResult::Matches;

    //    3. A is an ASCII case-insensitive match for "ws", and B is an ASCII case-insensitive match for "wss", "http", or "https".
    if (a.equals_ignoring_ascii_case("ws"sv)
        && (b.equals_ignoring_ascii_case("wss"sv)
            || b.equals_ignoring_ascii_case("http"sv)
            || b.equals_ignoring_ascii_case("https"sv))) {
        return MatchResult::Matches;
    }

    //    4. A is an ASCII case-insensitive match for "wss", and B is an ASCII case-insensitive match for "https".
    if (a.equals_ignoring_ascii_case("wss"sv) && b.equals_ignoring_ascii_case("https"sv))
        return MatchResult::Matches;

    // 2. Return "Does Not Match".
    return MatchResult::DoesNotMatch;
}

// https://w3c.github.io/webappsec-csp/#host-part-match
// An ASCII string host-part matches a host if a CSP source expression that contained the first as a host-part could
// potentially match the latter. For example, we say that "www.example.com" host-part matches "www.example.com".
// More formally, ASCII string pattern and host host are said to host-part match if the following algorithm returns "Matches":
// Spec Note: The matching relation is asymmetric. That is, pattern matching host does not mean that host will match pattern.
//            For example, *.example.com host-part matches www.example.com, but www.example.com does not host-part match *.example.com.
[[nodiscard]] static MatchResult host_part_matches(StringView pattern, Optional<URL::Host> const& maybe_host)
{
    // 1. If host is not a domain, return "Does Not Match".
    // Spec Note: A future version of this specification may allow literal IPv6 and IPv4 addresses, depending on usage and demand.
    //            Given the weak security properties of IP addresses in relation to named hosts, however, authors are encouraged
    //            to prefer the latter whenever possible.
    if (!maybe_host.has_value())
        return MatchResult::DoesNotMatch;

    auto const& host = maybe_host.value();

    if (!host.is_domain())
        return MatchResult::DoesNotMatch;

    // 2. If pattern is "*", return "Matches".
    if (pattern == "*"sv)
        return MatchResult::Matches;

    VERIFY(host.has<String>());
    auto host_string = host.get<String>();

    // 3. If pattern starts with "*.":
    if (pattern.starts_with("*."sv)) {
        // 1. Let remaining be pattern with the leading U+002A (*) removed and ASCII lowercased.
        auto remaining_without_asterisk = pattern.substring_view(1);
        auto remaining = remaining_without_asterisk.to_ascii_lowercase_string();

        // 2. If host to ASCII lowercase ends with remaining, then return "Matches".
        auto lowercase_host = host_string.to_ascii_lowercase();
        if (lowercase_host.ends_with_bytes(remaining))
            return MatchResult::Matches;

        // 3. Return "Does Not Match".
        return MatchResult::DoesNotMatch;
    }

    // 4. If pattern is not an ASCII case-insensitive match for host, return "Does Not Match".
    if (!pattern.equals_ignoring_ascii_case(host_string))
        return MatchResult::DoesNotMatch;

    // 5. Return "Matches".
    return MatchResult::Matches;
}

// https://w3c.github.io/webappsec-csp/#port-part-matches
// An ASCII string input port-part matches URL url if a CSP source expression that contained the first as a port-part
// could potentially match a URL containing the latter’s port and scheme. For example, "80" port-part matches
// matches http://example.com.
[[nodiscard]] static MatchResult port_part_matches(Optional<StringView> input, URL::URL const& url)
{
    // FIXME: 1. Assert: input is the empty string, "*", or a sequence of ASCII digits.

    // 2. If input is equal to "*", return "Matches".
    if (input == "*"sv)
        return MatchResult::Matches;

    // 3. Let normalizedInput be null if input is the empty string; otherwise input interpreted as decimal number.
    Optional<u16> normalized_input;
    if (input.has_value()) {
        VERIFY(!input.value().is_empty());
        auto maybe_port = input.value().to_number<u16>(TrimWhitespace::No);

        // If the port is empty here, then it's because the input overflowed the u16. Since this means it's bigger than
        // a u16, it can never match the URL's port, which is only within the u16 range.
        if (!maybe_port.has_value())
            return MatchResult::DoesNotMatch;

        normalized_input = maybe_port.value();
    }

    // 4. If normalizedInput equals url’s port, return "Matches".
    if (normalized_input == url.port())
        return MatchResult::Matches;

    // 5. If url’s port is null:
    if (!url.port().has_value()) {
        // 1. Let defaultPort be the default port for url’s scheme.
        auto default_port = URL::default_port_for_scheme(url.scheme());

        // 2. If normalizedInput equals defaultPort, return "Matches".
        if (normalized_input == default_port)
            return MatchResult::Matches;
    }

    // 6. Return "Does Not Match".
    return MatchResult::DoesNotMatch;
}

// https://w3c.github.io/webappsec-csp/#path-part-match
// An ASCII string path A path-part matches another ASCII string path B if a CSP source expression that contained the
// first as a path-part could potentially match a URL containing the latter as a path. For example, we say that
// "/subdirectory/" path-part matches "/subdirectory/file".
// Spec Note: The matching relation is asymmetric. That is, path A matching path B does not mean that path B will
//            match path A.
[[nodiscard]] static MatchResult path_part_matches(StringView a, StringView b)
{
    // 1. If path A is the empty string, return "Matches".
    if (a.is_empty())
        return MatchResult::Matches;

    // 2. If path A consists of one character that is equal to the U+002F SOLIDUS character (/) and path B is the empty
    //    string, return "Matches".
    if (a == "/"sv && b.is_empty())
        return MatchResult::Matches;

    // 3. Let exact match be false if the final character of path A is the U+002F SOLIDUS character (/), and true
    //    otherwise.
    auto exact_match = !a.ends_with('/');

    // 4. Let path list A and path list B be the result of strictly splitting path A and path B respectively on the
    //    U+002F SOLIDUS character (/).
    auto path_list_a = a.split_view('/', SplitBehavior::KeepEmpty);
    auto path_list_b = b.split_view('/', SplitBehavior::KeepEmpty);

    // 5. If path list A has more items than path list B, return "Does Not Match".
    if (path_list_a.size() > path_list_b.size())
        return MatchResult::DoesNotMatch;

    // 6. If exact match is true, and path list A does not have the same number of items as path list B,
    //    return "Does Not Match".
    if (exact_match && path_list_a.size() != path_list_b.size())
        return MatchResult::DoesNotMatch;

    // 7. If exact match is false:
    if (!exact_match) {
        // 1. Assert: the final item in path list A is the empty string.
        VERIFY(path_list_a.last().is_empty());

        // 2. Remove the final item from path list A.
        (void)path_list_a.take_last();
    }

    // 8. For each piece A of path list A:
    for (size_t path_set_a_index = 0; path_set_a_index < path_list_a.size(); ++path_set_a_index) {
        auto piece_a = path_list_a[path_set_a_index];

        // 1. Let piece B be the next item in path list B.
        auto piece_b = path_list_b[path_set_a_index];

        // 2. Let decoded piece A be the percent-decoding of piece A.
        auto decoded_piece_a = URL::percent_decode(piece_a);

        // 3. Let decoded piece B be the percent-decoding of piece B.
        auto decoded_piece_b = URL::percent_decode(piece_b);

        // 4. If decoded piece A is not decoded piece B, return "Does Not Match".
        if (decoded_piece_a != decoded_piece_b)
            return MatchResult::DoesNotMatch;
    }

    // 9. Return "Matches".
    return MatchResult::Matches;
}

// https://w3c.github.io/webappsec-csp/#match-url-to-source-expression
MatchResult does_url_match_expression_in_origin_with_redirect_count(URL::URL const& url, String const& expression, URL::Origin const& origin, u8 redirect_count)
{
    // Spec Note: origin is the origin of the resource relative to which the expression should be resolved.
    //            "'self'", for instance, will have distinct meaning depending on that bit of context.

    // 1. If expression is the string "*", return "Matches" if one or more of the following conditions is met:
    //    1. url’s scheme is an HTTP(S) scheme.
    //    2. url’s scheme is the same as origin’s scheme.
    // Spec Note: This logic means that in order to allow a resource from a non-HTTP(S) scheme, it has to be either
    //            explicitly specified (e.g. default-src * data: custom-scheme-1: custom-scheme-2:), or the protected
    //            resource must be loaded from the same scheme.
    StringView origin_scheme {};
    if (!origin.is_opaque() && origin.scheme().has_value())
        origin_scheme = origin.scheme()->bytes_as_string_view();

    if (expression == "*"sv && (Fetch::Infrastructure::is_http_or_https_scheme(url.scheme()) || url.scheme() == origin_scheme))
        return MatchResult::Matches;

    // 2. If expression matches the scheme-source or host-source grammar:
    auto scheme_source_parse_result = parse_source_expression(Production::SchemeSource, expression);
    auto host_source_parse_result = parse_source_expression(Production::HostSource, expression);
    if (scheme_source_parse_result.has_value() || host_source_parse_result.has_value()) {
        // 1. If expression has a scheme-part, and it does not scheme-part match url’s scheme, return "Does Not Match".
        auto maybe_scheme_part = scheme_source_parse_result.has_value()
            ? scheme_source_parse_result->scheme_part
            : host_source_parse_result->scheme_part;

        if (maybe_scheme_part.has_value()) {
            if (scheme_part_matches(maybe_scheme_part.value(), url.scheme()) == MatchResult::DoesNotMatch)
                return MatchResult::DoesNotMatch;
        }

        // 2. If expression matches the scheme-source grammar, return "Matches".
        if (scheme_source_parse_result.has_value())
            return MatchResult::Matches;
    }

    // 3. If expression matches the host-source grammar:
    if (host_source_parse_result.has_value()) {
        // 1. If url’s host is null, return "Does Not Match".
        if (!url.host().has_value())
            return MatchResult::DoesNotMatch;

        // 2. If expression does not have a scheme-part, and origin’s scheme does not scheme-part match url’s scheme,
        //    return "Does Not Match".
        // Spec Note: As with scheme-part above, we allow schemeless host-source expressions to be upgraded from
        //            insecure schemes to secure schemes.
        if (!host_source_parse_result->scheme_part.has_value() && scheme_part_matches(origin_scheme, url.scheme()) == MatchResult::DoesNotMatch)
            return MatchResult::DoesNotMatch;

        // 3. If expression’s host-part does not host-part match url’s host, return "Does Not Match".
        VERIFY(host_source_parse_result->host_part.has_value());
        if (host_part_matches(host_source_parse_result->host_part.value(), url.host()) == MatchResult::DoesNotMatch)
            return MatchResult::DoesNotMatch;

        // 4. Let port-part be expression’s port-part if present, and null otherwise.
        auto port_part = host_source_parse_result->port_part;

        // 5. If port-part does not port-part match url, return "Does Not Match".
        if (port_part_matches(port_part, url) == MatchResult::DoesNotMatch)
            return MatchResult::DoesNotMatch;

        // 6. If expression contains a non-empty path-part, and redirect count is 0, then:
        if (host_source_parse_result->path_part.has_value() && !host_source_parse_result->path_part->is_empty() && redirect_count == 0) {
            // 1. Let path be the resulting of joining url’s path on the U+002F SOLIDUS character (/).
            // FIXME: File spec issue that if path_part is only '/', then plainly joining will always fail to match.
            //        It should likely use the URL path serializer instead.
            StringBuilder builder;
            builder.append('/');
            builder.join('/', url.paths());
            auto path = MUST(builder.to_string());

            // 2. If expression’s path-part does not path-part match path, return "Does Not Match".
            if (path_part_matches(host_source_parse_result->path_part.value(), path) == MatchResult::DoesNotMatch)
                return MatchResult::DoesNotMatch;
        }

        // 7. Return "Matches".
        return MatchResult::Matches;
    }

    // 4. If expression is an ASCII case-insensitive match for "'self'", return "Matches" if one or more of the
    //    following conditions is met:
    // Spec Note: Like the scheme-part logic above, the "'self'" matching algorithm allows upgrades to secure schemes
    //            when it is safe to do so. We limit these upgrades to endpoints running on the default port for a
    //            particular scheme or a port that matches the origin of the protected resource, as this seems
    //            sufficient to deal with upgrades that can be reasonably expected to succeed.
    if (expression.equals_ignoring_ascii_case(KeywordSources::Self)) {
        // 1. origin is the same as url’s origin
        if (origin.is_same_origin(url.origin()))
            return MatchResult::Matches;

        // 2. origin’s host is the same as url’s host, origin’s port and url’s port are either the same or the default
        //    ports for their respective schemes, and one or more of the following conditions is met:
        auto origin_default_port = URL::default_port_for_scheme(origin_scheme);
        auto url_default_port = URL::default_port_for_scheme(url.scheme());

        Optional<URL::Host> origin_host;
        Optional<u16> origin_port;

        if (!origin.is_opaque()) {
            origin_host = origin.host();
            origin_port = origin.port();
        }

        if (origin_host == url.host() && (origin.port() == url.port() || (origin_port == origin_default_port && url.port() == url_default_port))) {
            // 1. url’s scheme is "https" or "wss"
            if (url.scheme() == "https"sv || url.scheme() == "wss"sv)
                return MatchResult::Matches;

            // 2. origin’s scheme is "http" and url’s scheme is "http" or "ws"
            if (origin_scheme == "http"sv && (url.scheme() == "http"sv || url.scheme() == "ws"sv))
                return MatchResult::Matches;
        }
    }

    // 5. Return "Does Not Match".
    return MatchResult::DoesNotMatch;
}

// https://w3c.github.io/webappsec-csp/#match-url-to-source-list
MatchResult does_url_match_source_list_in_origin_with_redirect_count(URL::URL const& url, Vector<String> const& source_list, URL::Origin const& origin, u8 redirect_count)
{
    // 1. Assert: source list is not null.
    // NOTE: Already done by source_list being passed by reference.

    // 2. If source list is empty, return "Does Not Match".
    // Spec Note: An empty source list (that is, a directive without a value: script-src, as opposed to script-src host1)
    //            is equivalent to a source list containing 'none', and will not match any URL.
    if (source_list.is_empty())
        return MatchResult::DoesNotMatch;

    // 3. If source list’s size is 1, and source list[0] is an ASCII case-insensitive match for the string "'none'",
    //    return "Does Not Match".
    // Spec Note: The 'none' keyword has no effect when other source expressions are present. That is, the list « 'none' »
    //            does not match any URL. A list consisting of « 'none', https://example.com », on the other hand, would
    //            match https://example.com/.
    if (source_list.size() == 1 && source_list.first().equals_ignoring_ascii_case("'none'"sv))
        return MatchResult::DoesNotMatch;

    // 4. For each expression of source list:
    for (auto const& expression : source_list) {
        // 1. If § 6.7.2.8 Does url match expression in origin with redirect count? returns "Matches" when executed
        //    upon url, expression, origin, and redirect count, return "Matches".
        if (does_url_match_expression_in_origin_with_redirect_count(url, expression, origin, redirect_count) == MatchResult::Matches)
            return MatchResult::Matches;
    }

    // 5. Return "Does Not Match".
    return MatchResult::DoesNotMatch;
}

}
