/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/Directives/KeywordSources.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/Directives/SourceExpression.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/SRI/SRI.h>
#include <LibWeb/SVG/SVGElement.h>

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
    auto const& directive_fallback_list = get_fetch_directive_fallback_list(move(effective_directive_name));

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
static MatchResult scheme_part_matches(StringView a, StringView b)
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
static MatchResult host_part_matches(StringView pattern, Optional<URL::Host> const& maybe_host)
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
static MatchResult port_part_matches(Optional<StringView> input, URL::URL const& url)
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
static MatchResult path_part_matches(StringView a, StringView b)
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

        if (origin_host == url.host() && (origin_port == url.port() || (origin_port == origin_default_port && url.port() == url_default_port))) {
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

// https://w3c.github.io/webappsec-csp/#match-request-to-source-list
MatchResult does_request_match_source_list(GC::Ref<Fetch::Infrastructure::Request const> request, Vector<String> const& source_list, GC::Ref<Policy const> policy)
{
    // Given a request request, a source list source list, and a policy policy, this algorithm returns the result of
    // executing § 6.7.2.7 Does url match source list in origin with redirect count? on request’s current url, source
    // list, policy’s self-origin, and request’s redirect count.
    // Spec Note: This is generally used in directives' pre-request check algorithms to verify that a given request is
    //            reasonable.
    return does_url_match_source_list_in_origin_with_redirect_count(request->current_url(), source_list, policy->self_origin(), request->redirect_count());
}

// https://w3c.github.io/webappsec-csp/#match-response-to-source-list
MatchResult does_response_match_source_list(GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ref<Fetch::Infrastructure::Request const> request, Vector<String> const& source_list, GC::Ref<Policy const> policy)
{
    // Given a request request, and a source list source list, and a policy policy, this algorithm returns the result
    // of executing § 6.7.2.7 Does url match source list in origin with redirect count? on response’s url, source list,
    // policy’s self-origin, and request’s redirect count.
    // Spec Note: This is generally used in directives' post-request check algorithms to verify that a given response
    //            is reasonable.
    // FIXME: File spec issue that it does specify to pass in response here.
    VERIFY(response->url().has_value());
    return does_url_match_source_list_in_origin_with_redirect_count(response->url().value(), source_list, policy->self_origin(), request->redirect_count());
}

// https://w3c.github.io/webappsec-csp/#match-nonce-to-source-list
MatchResult does_nonce_match_source_list(String const& nonce, Vector<String> const& source_list)
{
    // 1. Assert: source list is not null.
    // Already done by only accept references.

    // 2. If nonce is the empty string, return "Does Not Match".
    if (nonce.is_empty())
        return MatchResult::DoesNotMatch;

    // 3. For each expression of source list:
    for (auto const& expression : source_list) {
        // 1. If expression matches the nonce-source grammar, and nonce is identical to expression’s base64-value part,
        //    return "Matches".
        auto nonce_source_match_result = parse_source_expression(Production::NonceSource, expression);
        if (nonce_source_match_result.has_value()) {
            VERIFY(nonce_source_match_result->base64_value.has_value());
            if (nonce == nonce_source_match_result->base64_value.value())
                return MatchResult::Matches;
        }
    }

    // 4. Return "Does Not Match".
    return MatchResult::DoesNotMatch;
}

// https://w3c.github.io/webappsec-csp/#match-integrity-metadata-to-source-list
// Spec Note: Here, we verify only whether the integrity metadata is a non-empty subset of the hash-source sources in
//            source list. We rely on the browser’s enforcement of Subresource Integrity [SRI] to block non-matching
//            resources upon response.
static MatchResult does_integrity_metadata_match_source_list(String const& integrity_metadata, Vector<String> const& source_list)
{
    // 1. Assert: source list is not null.
    // NOTE: This is already done by passing in source_list by reference.

    // 2. Let integrity expressions be the set of source expressions in source list that match the hash-source grammar.
    Vector<SourceExpressionParseResult> integrity_expressions;
    for (auto const& expression : source_list) {
        auto hash_source_parse_result = parse_source_expression(Production::HashSource, expression);
        if (hash_source_parse_result.has_value())
            integrity_expressions.append(hash_source_parse_result.release_value());
    }

    // 3. If integrity expressions is empty, return "Does Not Match".
    if (integrity_expressions.is_empty())
        return MatchResult::DoesNotMatch;

    // 4. Let integrity sources be the result of executing the algorithm defined in SRI § 3.3.3 Parse metadata. on
    //    integrity metadata. [SRI]
    auto integrity_sources = MUST(SRI::parse_metadata(integrity_metadata));

    // 5. If integrity sources is "no metadata" or an empty set, return "Does Not Match".
    // FIXME: File a spec issue stating that this is targetting an older version of the SRI spec, which does not return
    //        "no metadata", but instead simply just returns an empty list if there is no metadata.
    //        The up-to-date spec is located at https://w3c.github.io/webappsec-subresource-integrity/
    if (integrity_sources.is_empty())
        return MatchResult::DoesNotMatch;

    // 6. For each source of integrity sources:
    for (auto const& source : integrity_sources) {
        // 1. If integrity expressions does not contain a source expression whose hash-algorithm is an ASCII
        //    case-insensitive match for source’s hash-algorithm, and whose base64-value is identical to source’s
        //    base64-value, return "Does Not Match".
        auto maybe_match = integrity_expressions.find_if([&source](auto const& integrity_expression) {
            VERIFY(integrity_expression.hash_algorithm.has_value());
            VERIFY(integrity_expression.base64_value.has_value());

            return integrity_expression.hash_algorithm.value().equals_ignoring_ascii_case(source.algorithm)
                && integrity_expression.base64_value.value() == source.base64_value;
        });

        if (maybe_match.is_end())
            return MatchResult::DoesNotMatch;
    }

    // 7. Return "Matches".
    return MatchResult::Matches;
}

// https://w3c.github.io/webappsec-csp/#script-pre-request
Directive::Result script_directives_pre_request_check(GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Directive const> directive, GC::Ref<Policy const> policy)
{
    // 1. If request’s destination is script-like:
    if (request->destination_is_script_like()) {
        // 1. If the result of executing § 6.7.2.3 Does nonce match source list? on request’s cryptographic nonce
        //    metadata and this directive’s value is "Matches", return "Allowed".
        if (does_nonce_match_source_list(request->cryptographic_nonce_metadata(), directive->value()) == MatchResult::Matches)
            return Directive::Result::Allowed;

        // 2. If the result of executing § 6.7.2.4 Does integrity metadata match source list? on request’s integrity
        //    metadata and this directive’s value is "Matches", return "Allowed".
        if (does_integrity_metadata_match_source_list(request->integrity_metadata(), directive->value()) == MatchResult::Matches)
            return Directive::Result::Allowed;

        // 3. If directive’s value contains a source expression that is an ASCII case-insensitive match for the
        //    "'strict-dynamic'" keyword-source:
        // Spec Note: "'strict-dynamic'" is explained in more detail in § 8.2 Usage of "'strict-dynamic'".
        //            https://w3c.github.io/webappsec-csp/#strict-dynamic-usage
        auto maybe_strict_dynamic = directive->value().find_if([](auto const& directive_value) {
            return directive_value.equals_ignoring_ascii_case(KeywordSources::StrictDynamic);
        });

        if (!maybe_strict_dynamic.is_end()) {
            // 1. If the request’s parser metadata is "parser-inserted", return "Blocked".
            //    Otherwise, return "Allowed".
            if (request->parser_metadata() == Fetch::Infrastructure::Request::ParserMetadata::ParserInserted)
                return Directive::Result::Blocked;

            return Directive::Result::Allowed;
        }

        // 4. If the result of executing § 6.7.2.5 Does request match source list? on request, directive’s value, and
        //    policy, is "Does Not Match", return "Blocked".
        if (does_request_match_source_list(request, directive->value(), policy) == MatchResult::DoesNotMatch)
            return Directive::Result::Blocked;
    }

    // 2. Return "Allowed".
    return Directive::Result::Allowed;
}

// https://w3c.github.io/webappsec-csp/#script-post-request
Directive::Result script_directives_post_request_check(GC::Ref<Fetch::Infrastructure::Request const> request, GC::Ref<Fetch::Infrastructure::Response const> response, GC::Ref<Directive const> directive, GC::Ref<Policy const> policy)
{
    // 1. If request’s destination is script-like:
    if (request->destination_is_script_like()) {
        // 1. If the result of executing § 6.7.2.3 Does nonce match source list? on request’s cryptographic nonce
        //    metadata and this directive’s value is "Matches", return "Allowed".
        if (does_nonce_match_source_list(request->cryptographic_nonce_metadata(), directive->value()) == MatchResult::Matches)
            return Directive::Result::Allowed;

        // 2. If the result of executing § 6.7.2.4 Does integrity metadata match source list? on request’s integrity
        //    metadata and this directive’s value is "Matches", return "Allowed".
        if (does_integrity_metadata_match_source_list(request->integrity_metadata(), directive->value()) == MatchResult::Matches)
            return Directive::Result::Allowed;

        // 3. If directive’s value contains "'strict-dynamic'":
        // FIXME: Should this be case insensitive?
        auto maybe_strict_dynamic = directive->value().find_if([](auto const& directive_value) {
            return directive_value.equals_ignoring_ascii_case(KeywordSources::StrictDynamic);
        });

        if (!maybe_strict_dynamic.is_end()) {
            // 1. If request’s parser metadata is not "parser-inserted", return "Allowed".
            //    Otherwise, return "Blocked".
            if (request->parser_metadata() != Fetch::Infrastructure::Request::ParserMetadata::ParserInserted)
                return Directive::Result::Allowed;

            return Directive::Result::Blocked;
        }

        // 4. If the result of executing § 6.7.2.6 Does response to request match source list? on response, request,
        //    directive’s value, and policy, is "Does Not Match", return "Blocked".
        if (does_response_match_source_list(response, request, directive->value(), policy) == MatchResult::DoesNotMatch)
            return Directive::Result::Blocked;
    }

    // 2. Return "Allowed".
    return Directive::Result::Allowed;
}

enum class [[nodiscard]] AllowsResult {
    DoesNotAllow,
    Allows,
};

static AllowsResult does_a_source_list_allow_all_inline_behavior_for_type(Vector<String> const& source_list, Directive::InlineType type)
{
    // 1. Let allow all inline be false.
    bool allow_all_inline = false;

    // 2. For each expression of list:
    for (auto const& expression : source_list) {
        // 1. If expression matches the nonce-source or hash-source grammar, return "Does Not Allow".
        auto nonce_source_parse_result = parse_source_expression(Production::NonceSource, expression);
        if (nonce_source_parse_result.has_value())
            return AllowsResult::DoesNotAllow;

        auto hash_source_parse_result = parse_source_expression(Production::HashSource, expression);
        if (hash_source_parse_result.has_value())
            return AllowsResult::DoesNotAllow;

        // 2. If type is "script", "script attribute" or "navigation" and expression matches the keyword-source
        //    "'strict-dynamic'", return "Does Not Allow".
        if (type == Directive::InlineType::Script || type == Directive::InlineType::ScriptAttribute || type == Directive::InlineType::Navigation) {
            if (expression.equals_ignoring_ascii_case(KeywordSources::StrictDynamic))
                return AllowsResult::DoesNotAllow;
        }

        // 3. If expression is an ASCII case-insensitive match for the keyword-source "'unsafe-inline'", set allow all
        //    inline to true.
        if (expression.equals_ignoring_ascii_case(KeywordSources::UnsafeInline))
            allow_all_inline = true;
    }

    // 3. If allow all inline is true, return "Allows". Otherwise, return "Does Not Allow".
    return allow_all_inline ? AllowsResult::Allows : AllowsResult::DoesNotAllow;
}

enum class NonceableResult {
    NotNonceable,
    Nonceable,
};

// https://w3c.github.io/webappsec-csp/#is-element-nonceable
[[nodiscard]] static NonceableResult is_element_nonceable(GC::Ptr<DOM::Element const> element)
{
    // SPEC ISSUE 7: This processing is meant to mitigate the risk of dangling markup attacks that steal the nonce from
    //               an existing element in order to load injected script. It is fairly expensive, however, as it
    //               requires that we walk through all attributes and their values in order to determine whether the
    //               script should execute. Here, we try to minimize the impact by doing this check only for script
    //               elements when a nonce is present, but we should probably consider this algorithm as "at risk"
    //               until we know its impact. [Issue #w3c/webappsec-csp#98] (https://github.com/w3c/webappsec-csp/issues/98)

    // FIXME: See FIXME in `does_element_match_source_list_for_type_and_source`
    if (!element)
        return NonceableResult::NotNonceable;

    // 1. If element does not have an attribute named "nonce", return "Not Nonceable".
    if (!is<HTML::HTMLElement>(element.ptr()) && !is<SVG::SVGElement>(element.ptr()))
        return NonceableResult::NotNonceable;

    if (!element->has_attribute(HTML::AttributeNames::nonce))
        return NonceableResult::NotNonceable;

    // 2. If element is a script element, then for each attribute of element’s attribute list:
    // FIXME: File spec issue to ask if this should include SVGScriptElement.
    if (is<HTML::HTMLScriptElement>(element.ptr())) {
        for (size_t attribute_index = 0; attribute_index < element->attributes()->length(); ++attribute_index) {
            auto const* attribute = element->attributes()->item(attribute_index);
            VERIFY(attribute);

            // 1. If attribute’s name contains an ASCII case-insensitive match for "<script" or "<style", return
            //    "Not Nonceable".
            auto attribute_name = attribute->name().to_string();
            if (attribute_name.contains("<script"sv, CaseSensitivity::CaseInsensitive) || attribute_name.contains("<style"sv, CaseSensitivity::CaseInsensitive))
                return NonceableResult::NotNonceable;

            // 2. If attribute’s value contains an ASCII case-insensitive match for "<script" or "<style", return
            //    "Not Nonceable".
            auto const& attribute_value = attribute->value();
            if (attribute_value.contains("<script"sv, CaseSensitivity::CaseInsensitive) || attribute_value.contains("<style"sv, CaseSensitivity::CaseInsensitive))
                return NonceableResult::NotNonceable;
        }
    }

    // 3. If element had a duplicate-attribute parse error during tokenization, return "Not Nonceable".
    // SPEC ISSUE 6: We need some sort of hook in HTML to record this error if we’re planning on using it here.
    //               [Issue #whatwg/html#3257] (https://github.com/whatwg/html/issues/3257)
    if (element->had_duplicate_attribute_during_tokenization())
        return NonceableResult::NotNonceable;

    // 4. Return "Nonceable".
    return NonceableResult::Nonceable;
}

// https://w3c.github.io/webappsec-csp/#match-element-to-source-list
MatchResult does_element_match_source_list_for_type_and_source(GC::Ptr<DOM::Element const> element, Vector<String> const& source_list, Directive::InlineType type, String const& source)
{
    // Spec Note: Regardless of the encoding of the document, source will be converted to UTF-8 before applying any
    //            hashing algorithms.

    // 1. If § 6.7.3.2 Does a source list allow all inline behavior for type? returns "Allows" given list and type,
    //    return "Matches".
    if (does_a_source_list_allow_all_inline_behavior_for_type(source_list, type) == AllowsResult::Allows)
        return MatchResult::Matches;

    // 2. If type is "script" or "style", and § 6.7.3.1 Is element nonceable? returns "Nonceable" when executed upon
    //    element:
    // Spec Note: Nonces only apply to inline script and inline style, not to attributes of either element or to
    //            javascript: navigations.
    // FIXME: File spec issue that this algorithm doesn't handle `element` being null, which is it when doing a
    //        javascript: URL navigation. For now, we say that the element is not nonceable if it's null, because
    //        we simply can't pull a nonce attribute value from a null element.
    if ((type == Directive::InlineType::Script || type == Directive::InlineType::Style) && is_element_nonceable(element) == NonceableResult::Nonceable) {
        // 1. For each expression of list:
        for (auto const& expression : source_list) {
            // 1. If expression matches the nonce-source grammar, and element has a nonce attribute whose value is
            //    expression's base64-value part, return "Matches".
            auto nonce_source_parse_result = parse_source_expression(Production::NonceSource, expression);
            if (nonce_source_parse_result.has_value()) {
                VERIFY(element);
                VERIFY(is<HTML::HTMLElement>(element.ptr()) || is<SVG::SVGElement>(element.ptr()));

                String element_nonce;
                if (auto* html_element = as_if<HTML::HTMLElement>(element.ptr())) {
                    element_nonce = html_element->nonce();
                } else {
                    auto const& svg_element = as<SVG::SVGElement>(*element);
                    element_nonce = svg_element.nonce();
                }

                if (nonce_source_parse_result->base64_value == element_nonce)
                    return MatchResult::Matches;
            }
        }
    }

    // 3. Let unsafe-hashes flag be false.
    bool unsafe_hashes_flag = false;

    // 4. For each expression of list:
    for (auto const& expression : source_list) {
        // 1. If expression is an ASCII case-insensitive match for the keyword-source "'unsafe-hashes'", set
        //    unsafe-hashes flag to true. Break out of the loop.
        if (expression.equals_ignoring_ascii_case(KeywordSources::UnsafeHashes)) {
            unsafe_hashes_flag = true;
            break;
        }
    }

    // 5. If type is "script" or "style", or unsafe-hashes flag is true:
    // NOTE: Hashes apply to inline script and inline style. If the "'unsafe-hashes'" source expression is present,
    //       they will also apply to event handlers, style attributes and javascript: navigations.
    // SPEC ISSUE 8:  This should handle 'strict-dynamic' for dynamically inserted inline scripts.
    //                [Issue #w3c/webappsec-csp#426] (https://github.com/w3c/webappsec-csp/issues/426)
    if (type == Directive::InlineType::Script || type == Directive::InlineType::Style || unsafe_hashes_flag) {
        // 1. Set source to the result of executing UTF-8 encode on the result of executing JavaScript string
        //    converting on source.
        auto converted_source = MUST(Infra::convert_to_scalar_value_string(source));

        // NOTE: converted_source is already UTF-8 encoded.
        auto converted_source_bytes = converted_source.bytes();

        // 2. For each expression of list:
        for (auto const& expression : source_list) {
            // 1. If expression matches the hash-source grammar:
            auto hash_source_parse_result = parse_source_expression(Production::HashSource, expression);
            if (hash_source_parse_result.has_value()) {
                // 1. Let algorithm be null.
                StringView algorithm;

                // 2. If expression’s hash-algorithm part is an ASCII case-insensitive match for "sha256", set
                //    algorithm to SHA-256.
                VERIFY(hash_source_parse_result->hash_algorithm.has_value());
                auto hash_algorithm_from_expression = hash_source_parse_result->hash_algorithm.value();

                if (hash_algorithm_from_expression.equals_ignoring_ascii_case("sha256"sv))
                    algorithm = "SHA-256"sv;

                // 3. If expression’s hash-algorithm part is an ASCII case-insensitive match for "sha384", set
                //    algorithm to SHA-384.
                if (hash_algorithm_from_expression.equals_ignoring_ascii_case("sha384"sv))
                    algorithm = "SHA-384"sv;

                // 4. If expression’s hash-algorithm part is an ASCII case-insensitive match for "sha512", set
                //    algorithm to SHA-512.
                if (hash_algorithm_from_expression.equals_ignoring_ascii_case("sha512"sv))
                    algorithm = "SHA-512"sv;

                // 5. If algorithm is not null:
                if (!algorithm.is_null()) {
                    // 1. Let actual be the result of base64 encoding the result of applying algorithm to source.
                    auto apply_algorithm_to_source = [&] {
                        if (algorithm == "SHA-256"sv) {
                            auto result = ::Crypto::Hash::SHA256::hash(converted_source_bytes);
                            return MUST(encode_base64(result.bytes()));
                        }

                        if (algorithm == "SHA-384"sv) {
                            auto result = ::Crypto::Hash::SHA384::hash(converted_source_bytes);
                            return MUST(encode_base64(result.bytes()));
                        }

                        if (algorithm == "SHA-512"sv) {
                            auto result = ::Crypto::Hash::SHA512::hash(converted_source_bytes);
                            return MUST(encode_base64(result.bytes()));
                        }

                        VERIFY_NOT_REACHED();
                    };

                    auto actual = apply_algorithm_to_source();

                    // 2. Let expected be expression’s base64-value part, with all '-' characters replaced with '+',
                    //    and all '_' characters replaced with '/'.
                    // Spec Note: This replacement normalizes hashes expressed in base64url encoding into base64
                    //            encoding for matching.
                    VERIFY(hash_source_parse_result->base64_value.has_value());
                    auto base64_value_string = MUST(String::from_utf8(hash_source_parse_result->base64_value.value()));

                    auto expected = MUST(base64_value_string.replace("-"sv, "+"sv, ReplaceMode::All));
                    expected = MUST(expected.replace("_"sv, "/"sv, ReplaceMode::All));

                    // 3. If actual is identical to expected, return "Matches".
                    if (actual == expected)
                        return MatchResult::Matches;
                }
            }
        }
    }

    // 6. Return "Does Not Match".
    return MatchResult::DoesNotMatch;
}

}
