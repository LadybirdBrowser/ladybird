/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibCrypto/Hash/SHA1.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibHTTP/HTTP.h>
#include <LibURL/URL.h>

namespace HTTP {

// https://httpwg.org/specs/rfc9110.html#field.date
static Optional<UnixDateTime> parse_http_date(Optional<ByteString const&> date)
{
    // <day-name>, <day> <month> <year> <hour>:<minute>:<second> GMT
    if (date.has_value())
        return UnixDateTime::parse("%a, %d %b %Y %T GMT"sv, *date, true);
    return {};
}

u64 compute_maximum_disk_cache_size(u64 free_bytes, u64 limit_maximum_disk_cache_size)
{
    auto cache_size = [&]() {
        if (free_bytes <= 100 * MiB)
            return free_bytes * 8 / 10; // Up to 80 MiB
        if (free_bytes <= 800 * MiB)
            return free_bytes * 6 / 10; // Up to 480 MiB
        if (free_bytes <= 2 * GiB)
            return free_bytes * 4 / 10; // Up to 820 MiB
        if (free_bytes <= 10 * GiB)
            return free_bytes * 2 / 10; // Up to 2 GiB
        return limit_maximum_disk_cache_size;
    }();

    return min(cache_size, limit_maximum_disk_cache_size);
}

u64 compute_maximum_disk_cache_entry_size(u64 maximum_disk_cache_size)
{
    static constexpr u64 MAXIMUM_DISK_CACHE_ENTRY_SIZE = 256 * MiB;

    return min(maximum_disk_cache_size / 8, MAXIMUM_DISK_CACHE_ENTRY_SIZE);
}

String serialize_url_for_cache_storage(URL::URL const& url)
{
    if (!url.fragment().has_value())
        return url.serialize();

    auto sanitized = url;
    sanitized.set_fragment({});
    return sanitized.serialize();
}

static u64 serialize_hash(Crypto::Hash::SHA1& hasher)
{
    auto digest = hasher.digest();
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

u64 create_cache_key(StringView url, StringView method)
{
    auto hasher = Crypto::Hash::SHA1::create();
    hasher->update(url);
    hasher->update(method);

    return serialize_hash(*hasher);
}

u64 create_vary_key(HeaderList const& request_headers, HeaderList const& response_headers)
{
    auto hasher = Crypto::Hash::SHA1::create();
    auto has_vary_header = false;

    response_headers.for_each_vary_header([&](StringView header) {
        // If we start caching `Vary: *` responses, this needs to be updated.
        VERIFY(header != "*"sv);
        has_vary_header = true;

        auto value = normalize_request_vary_header_values(header, request_headers);
        hasher->update(value);

        return IterationDecision::Continue;
    });

    return has_vary_header ? serialize_hash(*hasher) : 0;
}

LexicalPath path_for_cache_entry(LexicalPath const& cache_directory, u64 cache_key, u64 vary_key)
{
    auto file = vary_key == 0
        ? ByteString::formatted("{:016x}", cache_key)
        : ByteString::formatted("{:016x}_{:016x}", cache_key, vary_key);

    return cache_directory.append(file);
}

// https://httpwg.org/specs/rfc9111.html#response.cacheability
bool is_cacheable(StringView method, HTTP::HeaderList const& request_headers)
{
    // A cache MUST NOT store a response to a request unless:

    // * the request method is understood by the cache;
    if (!method.is_one_of("GET"sv, "HEAD"sv))
        return false;

    auto cache_control = request_headers.get("Cache-Control"sv);

    // https://httpwg.org/specs/rfc9111.html#cache-request-directive.no-store
    // The no-store request directive indicates that a cache MUST NOT store any part of either this request or any
    // response to it.
    if (cache_control.has_value() && cache_control->contains("no-store"sv, CaseSensitivity::CaseInsensitive))
        return false;

    // FIXME: Neither the disk cache nor the memory cache handle partial responses yet. So we don't cache them for now.
    return !request_headers.contains("Range"sv);
}

// https://datatracker.ietf.org/doc/html/rfc9110#name-overview-of-status-codes
static bool is_heuristically_cacheable_status(u32 status_code)
{
    // Responses with status codes that are defined as heuristically cacheable
    // (e.g., 200, 203, 204, 206, 300, 301, 308, 404, 405, 410, 414, and 501)
    // can be reused by a cache with heuristic expiration [...]
    switch (status_code) {
    case 200:
    case 203:
    case 204:
    case 206:
    case 300:
    case 301:
    case 308:
    case 404:
    case 405:
    case 410:
    case 414:
    case 501:
        return true;
    default:
        return false;
    }
}

// https://httpwg.org/specs/rfc9111.html#response.cacheability
bool is_cacheable(u32 status_code, HeaderList const& headers)
{
    // A cache MUST NOT store a response to a request unless:

    // * the response status code is final (see Section 15 of [HTTP]);
    if (status_code < 200)
        return false;

    // FIXME: Neither the disk cache nor the memory cache handle partial responses yet. So we don't cache them for now.
    if (status_code == 206)
        return false;

    // FIXME: If the response contains `Vary: *`, we don't cache it for now, as it "signals that other aspects of the
    //        request might have played a role in selecting the response representation, possibly including aspects
    //        outside the message syntax". Rather than guessing which cached response might be a fit for a new request,
    //        we will issue an unconditional request for now.
    //        https://httpwg.org/specs/rfc9110.html#field.vary
    bool contains_vary_wildcard = false;

    headers.for_each_vary_header([&](StringView header) {
        if (header == "*"sv) {
            contains_vary_wildcard = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });

    if (contains_vary_wildcard)
        return false;

    auto cache_control = headers.get("Cache-Control"sv);

    // * if the response status code is 206 or 304, or the must-understand cache directive (see Section 5.2.2.3) is
    //   present: the cache understands the response status code;
    //
    // NB: This cache implements the semantics of 304 for revalidation. 206 is excluded above.
    bool has_must_understand = cache_control.has_value() && contains_cache_control_directive(*cache_control, "must-understand"sv);

    if (has_must_understand) {
        if (!is_heuristically_cacheable_status(status_code) && status_code != 304)
            return false;

        // https://httpwg.org/specs/rfc9111.html#cache-response-directive.must-understand
        // The must-understand response directive limits caching of the response to a cache that understands and conforms
        // to the requirements for that response's status code.
        //
        // A response that contains the must-understand directive SHOULD also contain the no-store directive. When a cache
        // that implements the must-understand directive receives a response that includes it, the cache SHOULD ignore the
        // no-store directive if it understands and implements the status code's caching requirements.
    } else {
        // * the no-store cache directive is not present in the response (see Section 5.2.2.5);
        if (cache_control.has_value() && contains_cache_control_directive(*cache_control, "no-store"sv))
            return false;
    }

    // * if the cache is shared: the private response directive is either not present or allows a shared cache to store
    //   a modified response; see Section 5.2.2.7);
    //
    // Not applicable: this is a private UA cache.

    // * if the cache is shared: the Authorization header field is not present in the request (see Section 11.6.2 of
    //   [HTTP]) or a response directive is present that explicitly allows shared caching (see Section 3.5); and
    //
    // Not applicable: this is a private UA cache.

    // * the response contains at least one of the following:
    //     - a public response directive (see Section 5.2.2.9);
    //     - a private response directive, if the cache is not shared (see Section 5.2.2.7);
    //     - an Expires header field (see Section 5.3);
    //     - a max-age response directive (see Section 5.2.2.1);
    //     - if the cache is shared: an s-maxage response directive (see Section 5.2.2.10);
    //     - a cache extension that allows it to be cached (see Section 5.2.3); or
    //     - a status code that is defined as heuristically cacheable (see Section 4.2.2).

    bool has_expires = headers.contains("Expires"sv);
    bool has_public = false;
    bool has_private = false;
    bool has_max_age = false;

    if (cache_control.has_value()) {
        has_public = contains_cache_control_directive(*cache_control, "public"sv);
        has_private = contains_cache_control_directive(*cache_control, "private"sv);
        has_max_age = contains_cache_control_directive(*cache_control, "max-age"sv);

        // FIXME: cache extensions that explicitly allow caching are not interpreted.
    }

    if (!has_public
        && !has_private
        && !has_expires
        && !has_max_age
        && !is_heuristically_cacheable_status(status_code)) {
        return false;
    }

    // Note that, in normal operation, some caches will not store a response that has neither
    // a cache validator nor an explicit expiration time, as such responses are not usually
    // useful to store. However, caches are not prohibited from storing such responses.
    //
    // This function only answers whether storage is permitted by the protocol.
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
        "Upgrade"sv,

        // * Likewise, some fields' semantics require them to be removed before forwarding the message, and this MAY be
        //   implemented by doing so before storage; see Section 7.6.1 of [HTTP] for some examples.

        // * The no-cache (Section 5.2.2.4) and private (Section 5.2.2.7) cache directives can have arguments that
        //   prevent storage of header fields by all caches and shared caches, respectively.

        // * Header fields that are specific to the proxy that a cache uses when forwarding a request MUST NOT be stored,
        //   unless the cache incorporates the identity of the proxy into the cache key. Effectively, this is limited to
        //   Proxy-Authenticate (Section 11.7.1 of [HTTP]), Proxy-Authentication-Info (Section 11.7.3 of [HTTP]), and
        //   Proxy-Authorization (Section 11.7.2 of [HTTP]).

        // AD-HOC: Exclude headers used only for testing.
        TEST_CACHE_ENABLED_HEADER,
        TEST_CACHE_STATUS_HEADER,
        TEST_CACHE_REQUEST_TIME_OFFSET);
}

// https://httpwg.org/specs/rfc9111.html#heuristic.freshness
static AK::Duration calculate_heuristic_freshness_lifetime(HeaderList const& headers, AK::Duration current_time_offset_for_testing)
{
    // Since origin servers do not always provide explicit expiration times, a cache MAY assign a heuristic expiration
    // time when an explicit time is not specified, employing algorithms that use other field values (such as the
    // Last-Modified time) to estimate a plausible expiration time. This specification does not provide specific
    // algorithms, but it does impose worst-case constraints on their results.
    //
    // A cache MUST NOT use heuristics to determine freshness when an explicit expiration time is present in the stored
    // response. Because of the requirements in Section 3, heuristics can only be used on responses without explicit
    // freshness whose status codes are defined as heuristically cacheable and on responses without explicit freshness
    // that have been marked as explicitly cacheable (e.g., with a public response directive).
    //
    // If the response has a Last-Modified header field, caches are encouraged to use a heuristic expiration value that
    // is no more than some fraction of the interval since that time. A typical setting of this fraction might be 10%.

    auto last_modified = parse_http_date(headers.get("Last-Modified"sv));
    if (!last_modified.has_value())
        return {};

    auto now = UnixDateTime::now() + current_time_offset_for_testing;
    auto since_last_modified = now - *last_modified;
    auto seconds = since_last_modified.to_seconds();

    if (seconds <= 0)
        return {};

    // 10% heuristic, clamped at >= 0.
    auto heuristic_seconds = max<i64>(0, seconds / 10);
    return AK::Duration::from_seconds(heuristic_seconds);
}

// https://httpwg.org/specs/rfc9111.html#calculating.freshness.lifetime
AK::Duration calculate_freshness_lifetime(u32 status_code, HeaderList const& headers, AK::Duration current_time_offset_for_testing)
{
    // A cache can calculate the freshness lifetime (denoted as freshness_lifetime) of a response by evaluating the
    // following rules and using the first match:

    auto cache_control = headers.get("Cache-Control"sv);

    // * If the cache is shared and the s-maxage response directive (Section 5.2.2.10) is present, use its value, or
    //
    // Not a shared cache; s-maxage is ignored here.

    // * If the max-age response directive (Section 5.2.2.1) is present, use its value, or
    if (cache_control.has_value()) {
        if (auto max_age = extract_cache_control_duration_directive(*cache_control, "max-age"sv); max_age.has_value())
            return *max_age;
    }

    // * If the Expires response header field (Section 5.3) is present, use its value minus the value of the Date response
    //   header field (using the time the message was received if it is not present, as per Section 6.6.1 of [HTTP]), or
    if (auto expires = parse_http_date(headers.get("Expires"sv)); expires.has_value()) {
        auto date = parse_http_date(headers.get("Date"sv)).value_or_lazy_evaluated([&]() {
            return UnixDateTime::now() + current_time_offset_for_testing;
        });

        return *expires - date;
    }

    // * Otherwise, no explicit expiration time is present in the response. A heuristic freshness lifetime might be
    //   applicable; see Section 4.2.2.

    bool heuristics_allowed = false;

    // Because of the requirements in Section 3, heuristics can only be used on responses without explicit freshness
    // whose status codes are defined as heuristically cacheable and on responses without explicit freshness that have
    // been marked as explicitly cacheable (e.g., with a public response directive).
    if (is_heuristically_cacheable_status(status_code)) {
        heuristics_allowed = true;
    } else if (cache_control.has_value() && contains_cache_control_directive(*cache_control, "public"sv)) {
        heuristics_allowed = true;
    }

    if (heuristics_allowed)
        return calculate_heuristic_freshness_lifetime(headers, current_time_offset_for_testing);

    // No explicit expiration time, and heuristics not allowed or not applicable.
    return {};
}

// https://httpwg.org/specs/rfc9111.html#age.calculations
AK::Duration calculate_age(HeaderList const& headers, UnixDateTime request_time, UnixDateTime response_time, AK::Duration current_time_offset_for_testing)
{
    // The term "age_value" denotes the value of the Age header field (Section 5.1), in a form appropriate for arithmetic
    // operation; or 0, if not available.
    AK::Duration age_value;

    if (auto age = headers.get("Age"sv); age.has_value()) {
        if (auto seconds = age->to_number<i64>(); seconds.has_value())
            age_value = AK::Duration::from_seconds(*seconds);
    }

    // The term "now" means the current value of this implementation's clock (Section 5.6.7 of [HTTP]).
    auto now = UnixDateTime::now() + current_time_offset_for_testing;

    // The term "date_value" denotes the value of the Date header field, in a form appropriate for arithmetic operations.
    // See Section 6.6.1 of [HTTP] for the definition of the Date header field and for requirements regarding responses
    // without it.
    auto date_value = parse_http_date(headers.get("Date"sv)).value_or(now);

    auto apparent_age = max(AK::Duration::zero(), (response_time - date_value));

    auto response_delay = response_time - request_time;
    auto corrected_age_value = age_value + response_delay;

    auto corrected_initial_age = max(apparent_age, corrected_age_value);

    auto resident_time = now - response_time;
    auto current_age = corrected_initial_age + resident_time;

    return current_age;
}

// https://httpwg.org/specs/rfc5861.html#n-the-stale-while-revalidate-cache-control-extension
AK::Duration calculate_stale_while_revalidate_lifetime(HeaderList const& headers, AK::Duration freshness_lifetime)
{
    auto cache_control = headers.get("Cache-Control"sv);
    if (!cache_control.has_value())
        return {};

    if (auto swr = extract_cache_control_duration_directive(*cache_control, "stale-while-revalidate"sv); swr.has_value())
        return freshness_lifetime + *swr;
    return {};
}

CacheLifetimeStatus cache_lifetime_status(HeaderList const& request_headers, HeaderList const& response_headers, AK::Duration freshness_lifetime, AK::Duration current_age)
{
    auto revalidation_status = [&](auto revalidation_type) {
        // In order to revalidate a cache entry, we must have one of these headers to attach to the revalidation request.
        if (response_headers.contains("Last-Modified"sv) || response_headers.contains("ETag"sv))
            return revalidation_type;
        return CacheLifetimeStatus::Expired;
    };

    auto request_cache_control = request_headers.get("Cache-Control"sv);
    auto response_cache_control = response_headers.get("Cache-Control"sv);

    // https://httpwg.org/specs/rfc9111.html#cache-response-directive.no-cache
    // The no-cache response directive, in its unqualified form (without an argument), indicates that the response MUST
    // NOT be used to satisfy any other request without forwarding it for validation and receiving a successful response
    //
    // FIXME: Handle the qualified form of the no-cache directive, which may allow us to re-use the response.
    if (response_cache_control.has_value() && contains_cache_control_directive(*response_cache_control, "no-cache"sv))
        return revalidation_status(CacheLifetimeStatus::MustRevalidate);

    if (request_cache_control.has_value()) {
        // https://httpwg.org/specs/rfc9111.html#cache-request-directive.no-cache
        // The no-cache request directive indicates that the client prefers a stored response not be used to satisfy the
        // request without successful validation on the origin server.
        if (request_cache_control->contains("no-cache"sv, CaseSensitivity::CaseInsensitive))
            return revalidation_status(CacheLifetimeStatus::MustRevalidate);

        // https://httpwg.org/specs/rfc9111.html#cache-request-directive.max-age
        // The max-age request directive indicates that the client prefers a response whose age is less than or equal to
        // the specified number of seconds.
        if (auto max_age = extract_cache_control_duration_directive(*request_cache_control, "max-age"sv); max_age.has_value()) {
            if (*max_age <= current_age)
                return CacheLifetimeStatus::Expired;
        }

        // https://httpwg.org/specs/rfc9111.html#cache-request-directive.min-fresh
        // The min-fresh request directive indicates that the client prefers a response whose freshness lifetime is no
        // less than its current age plus the specified time in seconds. That is, the client wants a response that will
        // still be fresh for at least the specified number of seconds.
        if (auto min_fresh = extract_cache_control_duration_directive(*request_cache_control, "min-fresh"sv); min_fresh.has_value()) {
            if (freshness_lifetime < current_age + *min_fresh)
                return CacheLifetimeStatus::Expired;
        }
    }

    // https://httpwg.org/specs/rfc9111.html#expiration.model
    if (freshness_lifetime > current_age)
        return CacheLifetimeStatus::Fresh;

    if (request_cache_control.has_value()) {
        // https://httpwg.org/specs/rfc9111.html#cache-request-directive.max-stale
        // The max-stale request directive indicates that the client will accept a response that has exceeded its
        // freshness lifetime. If a value is present, then the client is willing to accept a response that has exceeded
        // its freshness lifetime by no more than the specified number of seconds. If no value is assigned to max-stale,
        // then the client will accept a stale response of any age.
        if (auto max_stale = extract_cache_control_duration_directive(*request_cache_control, "max-stale"sv, AK::Duration::max()); max_stale.has_value()) {
            if (freshness_lifetime + *max_stale > current_age)
                return CacheLifetimeStatus::Fresh;
        }
    }

    // AD-HOC: If there isn't a Cache-Control response header, we have already at least determined the response is
    //         heuristically cacheable by the time we reach here. Allow revalidating these responses. This is expected
    //         by WPT.
    if (!response_cache_control.has_value())
        return revalidation_status(CacheLifetimeStatus::MustRevalidate);

    // https://httpwg.org/specs/rfc5861.html#n-the-stale-while-revalidate-cache-control-extension
    // When present in an HTTP response, the stale-while-revalidate Cache-Control extension indicates that caches MAY
    // serve the response it appears in after it becomes stale, up to the indicated number of seconds.
    if (calculate_stale_while_revalidate_lifetime(response_headers, freshness_lifetime) > current_age)
        return revalidation_status(CacheLifetimeStatus::StaleWhileRevalidate);

    // https://httpwg.org/specs/rfc9111.html#cache-response-directive.must-revalidate
    // The must-revalidate response directive indicates that once the response has become stale, a cache MUST NOT reuse
    // that response to satisfy another request until it has been successfully validated by the origin
    if (contains_cache_control_directive(*response_cache_control, "must-revalidate"sv))
        return revalidation_status(CacheLifetimeStatus::MustRevalidate);

    return CacheLifetimeStatus::Expired;
}

// https://httpwg.org/specs/rfc9111.html#validation.sent
RevalidationAttributes RevalidationAttributes::create(HeaderList const& headers)
{
    RevalidationAttributes attributes;
    attributes.etag = headers.get("ETag"sv);
    attributes.last_modified = headers.get("Last-Modified"sv);

    return attributes;
}

// https://httpwg.org/specs/rfc9111.html#storing.fields
void store_header_and_trailer_fields(HeaderList& stored_headers, HeaderList const& response_headers)
{
    for (auto const& header : response_headers) {
        if (!is_header_exempted_from_storage(header.name))
            stored_headers.append(header);
    }
}

// https://httpwg.org/specs/rfc9111.html#update
void update_header_fields(HeaderList& stored_headers, HeaderList const& updated_headers)
{
    // Caches are required to update a stored response's header fields from another (typically newer) response in
    // several situations; for example, see Sections 3.4, 4.3.4, and 4.3.5.

    // When doing so, the cache MUST add each header field in the provided response to the stored response, replacing
    // field values that are already present, with the following exceptions:
    auto is_header_exempted_from_update = [](StringView name) {
        // * Header fields excepted from storage in Section 3.1,
        if (is_header_exempted_from_storage(name))
            return true;

        // * Header fields that the cache's stored response depends upon, as described below,
        // * Header fields that are automatically processed and removed by the recipient, as described below, and

        // * The Content-Length header field.
        if (name.equals_ignoring_ascii_case("Content-Length"sv))
            return true;

        return false;
    };

    for (auto const& updated_header : updated_headers) {
        if (!is_header_exempted_from_update(updated_header.name))
            stored_headers.delete_(updated_header.name);
    }

    for (auto const& updated_header : updated_headers) {
        if (!is_header_exempted_from_update(updated_header.name))
            stored_headers.append({ updated_header.name, updated_header.value });
    }
}

bool contains_cache_control_directive(StringView cache_control, StringView directive)
{
    return extract_cache_control_directive(cache_control, directive).has_value();
}

// This is a modified version of the "get, decode, and split" algorithm. This version stops at the first match found,
// does not un-escape quoted strings, and deals only with ASCII encodings. See:
// https://fetch.spec.whatwg.org/#header-value-get-decode-and-split
Optional<StringView> extract_cache_control_directive(StringView cache_control, StringView directive)
{
    VERIFY(!directive.is_empty());

    GenericLexer lexer { cache_control };
    size_t directive_start { 0 };

    while (true) {
        lexer.consume_until(is_any_of("\","sv));

        if (!lexer.is_eof() && lexer.peek() == '"') {
            auto quoted_string_start = lexer.tell();
            lexer.consume_quoted_string('\\');

            // FIXME: We currently bail if we come across an unterminated quoted string. Do other engines behave this
            //        way, or do they try to move on by finding the next comma?
            if (quoted_string_start == lexer.tell())
                return {};

            if (!lexer.is_eof())
                continue;
        }

        auto name = cache_control.substring_view(directive_start, lexer.tell() - directive_start);
        StringView value;

        if (auto index = name.find_any_of("=\""sv); index.has_value() && name[*index] == '=') {
            value = name.substring_view(*index + 1);
            name = name.substring_view(0, *index);
        }

        if (name.trim(HTTP_WHITESPACE).equals_ignoring_ascii_case(directive))
            return value.trim(HTTP_WHITESPACE);
        if (lexer.is_eof())
            return {};

        VERIFY(lexer.peek() == ',');
        lexer.ignore(1);

        directive_start = lexer.tell();
    }
}

Optional<AK::Duration> extract_cache_control_duration_directive(StringView cache_control, StringView directive, Optional<AK::Duration> valueless_fallback)
{
    if (auto value = extract_cache_control_directive(cache_control, directive); value.has_value()) {
        if (value->is_empty())
            return valueless_fallback;
        if (auto seconds = value->to_number<i64>(); seconds.has_value())
            return AK::Duration::from_seconds(*seconds);
    }

    return {};
}

// https://httpwg.org/specs/rfc9111.html#caching.negotiated.responses
ByteString normalize_request_vary_header_values(StringView header, HeaderList const& request_headers)
{
    // The header fields from two requests are defined to match if and only if those in the first request can be
    // transformed to those in the second request by applying any of the following:
    // * adding or removing whitespace, where allowed in the header field's syntax
    // * combining multiple header field lines with the same field name (see Section 5.2 of [HTTP])
    // * normalizing both header field values in a way that is known to have identical semantics, according to the
    //   header field's specification (e.g., reordering field values when order is not significant;
    //   case-normalization, where values are defined to be case-insensitive)
    StringBuilder builder;

    // FIXME: Find a definitive list of headers that are allowed to be normalized. The Cookie header, for example,
    //        cannot be normalized as order and case matters. So we err on the side of caution here.
    if (header.is_one_of_ignoring_ascii_case("Accept"sv, "Accept-Encoding"sv, "Accept-Language"sv)) {
        Vector<ByteString> values;

        request_headers.for_each_header_value(header, [&](ByteString value) {
            value = value.to_lowercase();

            if (!value.contains(',')) {
                values.append(move(value));
                return IterationDecision::Continue;
            }

            value.view().for_each_split_view(","sv, SplitBehavior::Nothing, [&](StringView field) {
                values.append(field.trim_whitespace());
            });
            return IterationDecision::Continue;
        });

        if (!values.is_empty()) {
            quick_sort(values);
            builder.join('\n', values);
        }
    } else {
        request_headers.for_each_header_value(header, [&](StringView value) {
            builder.append(value);
            builder.append('\n');
            return IterationDecision::Continue;
        });
    }

    return builder.to_byte_string();
}

AK::Duration compute_current_time_offset_for_testing(Optional<DiskCache&> disk_cache, HeaderList const& request_headers)
{
    if (disk_cache.has_value() && disk_cache->mode() == DiskCache::Mode::Testing) {
        if (auto header = request_headers.get(TEST_CACHE_REQUEST_TIME_OFFSET); header.has_value()) {
            if (auto offset = header->to_number<i64>(); offset.has_value())
                return AK::Duration::from_seconds(*offset);
        }
    }

    return {};
}

}
