/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Hash/SHA1.h>
#include <LibURL/URL.h>
#include <RequestServer/Cache/Utilities.h>

namespace RequestServer {

static Optional<StringView> extract_cache_control_directive(StringView cache_control, StringView directive)
{
    Optional<StringView> result;

    cache_control.for_each_split_view(","sv, SplitBehavior::Nothing, [&](StringView candidate) {
        if (!candidate.contains(directive, CaseSensitivity::CaseInsensitive))
            return IterationDecision::Continue;

        auto index = candidate.find('=');
        if (!index.has_value())
            return IterationDecision::Continue;

        result = candidate.substring_view(*index + 1);
        return IterationDecision::Break;
    });

    return result;
}

// https://httpwg.org/specs/rfc9110.html#field.date
static Optional<UnixDateTime> parse_http_date(Optional<ByteString const&> date)
{
    // <day-name>, <day> <month> <year> <hour>:<minute>:<second> GMT
    if (date.has_value())
        return UnixDateTime::parse("%a, %d %b %Y %T GMT"sv, *date, true);
    return {};
}

String serialize_url_for_cache_storage(URL::URL const& url)
{
    if (!url.fragment().has_value())
        return url.serialize();

    auto sanitized = url;
    sanitized.set_fragment({});
    return sanitized.serialize();
}

u64 create_cache_key(StringView url, StringView method)
{
    auto hasher = Crypto::Hash::SHA1::create();
    hasher->update(url);
    hasher->update(method);

    auto digest = hasher->digest();
    auto bytes = digest.bytes();

    u64 result = 0;
    result |= static_cast<u64>(bytes[0]) << 56;
    result |= static_cast<u64>(bytes[1]) << 48;
    result |= static_cast<u64>(bytes[2]) << 40;
    result |= static_cast<u64>(bytes[3]) << 32;
    result |= static_cast<u64>(bytes[4]) << 24;
    result |= static_cast<u64>(bytes[5]) << 16;
    result |= static_cast<u64>(bytes[6]) << 8;
    result |= static_cast<u64>(bytes[7]);

    return result;
}

// https://httpwg.org/specs/rfc9111.html#response.cacheability
bool is_cacheable(StringView method, u32 status_code, HTTP::HeaderMap const& headers)
{
    // A cache MUST NOT store a response to a request unless:

    // * the request method is understood by the cache;
    if (!method.is_one_of("GET"sv, "HEAD"sv))
        return false;

    // * the response status code is final (see Section 15 of [HTTP]);
    if (status_code < 200)
        return false;

    auto cache_control = headers.get("Cache-Control"sv);
    if (!cache_control.has_value())
        return false;

    // * if the response status code is 206 or 304, or the must-understand cache directive (see Section 5.2.2.3) is
    //   present: the cache understands the response status code;

    // * the no-store cache directive is not present in the response (see Section 5.2.2.5);
    if (cache_control->contains("no-store"sv, CaseSensitivity::CaseInsensitive))
        return false;

    // * if the cache is shared: the private response directive is either not present or allows a shared cache to store
    //   a modified response; see Section 5.2.2.7);

    // * if the cache is shared: the Authorization header field is not present in the request (see Section 11.6.2 of
    //   [HTTP]) or a response directive is present that explicitly allows shared caching (see Section 3.5); and

    // * the response contains at least one of the following:
    //     - a public response directive (see Section 5.2.2.9);
    //     - a private response directive, if the cache is not shared (see Section 5.2.2.7);
    //     - an Expires header field (see Section 5.3);
    //     - a max-age response directive (see Section 5.2.2.1);
    //     - if the cache is shared: an s-maxage response directive (see Section 5.2.2.10);
    //     - a cache extension that allows it to be cached (see Section 5.2.3); or
    //     - a status code that is defined as heuristically cacheable (see Section 4.2.2).

    // FIXME: Implement cache revalidation.
    if (cache_control->contains("no-cache"sv, CaseSensitivity::CaseInsensitive))
        return false;
    if (cache_control->contains("revalidate"sv, CaseSensitivity::CaseInsensitive))
        return false;

    return true;
}

// https://httpwg.org/specs/rfc9111.html#storing.fields
bool is_header_exempted_from_storage(StringView name)
{
    // Caches MUST include all received response header fields — including unrecognized ones — when storing a response;
    // this assures that new HTTP header fields can be successfully deployed. However, the following exceptions are made:
    return name.is_one_of_ignoring_ascii_case(
        // * The Connection header field and fields whose names are listed in it are required by Section 7.6.1 of [HTTP]
        //   to be removed before forwarding the message. This MAY be implemented by doing so before storage.
        "Connection"sv,
        "Keep-Alive"sv,
        "Proxy-Connection"sv,
        "TE"sv,
        "Transfer-Encoding"sv,
        "Upgrade"sv

        // * Likewise, some fields' semantics require them to be removed before forwarding the message, and this MAY be
        //   implemented by doing so before storage; see Section 7.6.1 of [HTTP] for some examples.

        // * The no-cache (Section 5.2.2.4) and private (Section 5.2.2.7) cache directives can have arguments that
        //   prevent storage of header fields by all caches and shared caches, respectively.

        // * Header fields that are specific to the proxy that a cache uses when forwarding a request MUST NOT be stored,
        //   unless the cache incorporates the identity of the proxy into the cache key. Effectively, this is limited to
        //   Proxy-Authenticate (Section 11.7.1 of [HTTP]), Proxy-Authentication-Info (Section 11.7.3 of [HTTP]), and
        //   Proxy-Authorization (Section 11.7.2 of [HTTP]).
    );
}

// https://httpwg.org/specs/rfc9111.html#calculating.freshness.lifetime
AK::Duration calculate_freshness_lifetime(HTTP::HeaderMap const& headers)
{
    // A cache can calculate the freshness lifetime (denoted as freshness_lifetime) of a response by evaluating the
    // following rules and using the first match:

    // * If the cache is shared and the s-maxage response directive (Section 5.2.2.10) is present, use its value, or

    // * If the max-age response directive (Section 5.2.2.1) is present, use its value, or
    if (auto cache_control = headers.get("Cache-Control"sv); cache_control.has_value()) {
        if (auto max_age = extract_cache_control_directive(*cache_control, "max-age"sv); max_age.has_value()) {
            if (auto seconds = max_age->to_number<i64>(); seconds.has_value())
                return AK::Duration::from_seconds(*seconds);
        }
    }

    // * If the Expires response header field (Section 5.3) is present, use its value minus the value of the Date response
    //   header field (using the time the message was received if it is not present, as per Section 6.6.1 of [HTTP]), or
    if (auto expires = parse_http_date(headers.get("Expires"sv)); expires.has_value()) {
        auto date = parse_http_date(headers.get("Date"sv)).value_or_lazy_evaluated([]() {
            return UnixDateTime::now();
        });

        return *expires - date;
    }

    // * Otherwise, no explicit expiration time is present in the response. A heuristic freshness lifetime might be
    //   applicable; see Section 4.2.2.

    return {};
}

// https://httpwg.org/specs/rfc9111.html#age.calculations
AK::Duration calculate_age(HTTP::HeaderMap const& headers, UnixDateTime request_time, UnixDateTime response_time)
{
    // The term "age_value" denotes the value of the Age header field (Section 5.1), in a form appropriate for arithmetic
    // operation; or 0, if not available.
    AK::Duration age_value;

    if (auto age = headers.get("Age"sv); age.has_value()) {
        if (auto seconds = age->to_number<i64>(); seconds.has_value())
            age_value = AK::Duration::from_seconds(*seconds);
    }

    // The term "now" means the current value of this implementation's clock (Section 5.6.7 of [HTTP]).
    auto now = UnixDateTime::now();

    // The term "date_value" denotes the value of the Date header field, in a form appropriate for arithmetic operations.
    // See Section 6.6.1 of [HTTP] for the definition of the Date header field and for requirements regarding responses
    // without it.
    auto date_value = parse_http_date(headers.get("Date"sv)).value_or(now);

    auto apparent_age = max(0LL, (response_time - date_value).to_seconds());

    auto response_delay = response_time - request_time;
    auto corrected_age_value = age_value + response_delay;

    auto corrected_initial_age = max(apparent_age, corrected_age_value.to_seconds());

    auto resident_time = (now - response_time).to_seconds();
    auto current_age = corrected_initial_age + resident_time;

    return AK::Duration::from_seconds(current_age);
}

// https://httpwg.org/specs/rfc9111.html#expiration.model
bool is_response_fresh(AK::Duration freshness_lifetime, AK::Duration current_age)
{
    return freshness_lifetime > current_age;
}

}
