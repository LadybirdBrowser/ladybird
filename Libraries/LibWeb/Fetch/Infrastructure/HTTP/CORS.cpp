/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibHTTP/Header.h>
#include <LibHTTP/HeaderList.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/CORS.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#cors-safelisted-request-header
bool is_cors_safelisted_request_header(HTTP::Header const& header)
{
    auto const& [name, value] = header;

    // 1. If value’s length is greater than 128, then return false.
    if (value.length() > 128)
        return false;

    // 2. Byte-lowercase name and switch on the result:
    // `accept`
    if (name.equals_ignoring_ascii_case("accept"sv)) {
        // If value contains a CORS-unsafe request-header byte, then return false.
        if (any_of(value, is_cors_unsafe_request_header_byte))
            return false;
    }
    // `accept-language`
    // `content-language`
    else if (name.is_one_of_ignoring_ascii_case("accept-language"sv, "content-language"sv)) {
        // If value contains a byte that is not in the range 0x30 (0) to 0x39 (9), inclusive, is not in the range 0x41 (A) to 0x5A (Z), inclusive, is not in the range 0x61 (a) to 0x7A (z), inclusive, and is not 0x20 (SP), 0x2A (*), 0x2C (,), 0x2D (-), 0x2E (.), 0x3B (;), or 0x3D (=), then return false.
        if (any_of(value, [](auto byte) {
                return !(is_ascii_digit(byte) || is_ascii_alpha(byte) || " *,-.;="sv.contains(byte));
            }))
            return false;
    }
    // `content-type`
    else if (name.equals_ignoring_ascii_case("content-type"sv)) {
        // 1. If value contains a CORS-unsafe request-header byte, then return false.
        if (any_of(value, is_cors_unsafe_request_header_byte))
            return false;

        // 2. Let mimeType be the result of parsing the result of isomorphic decoding value.
        auto decoded = TextCodec::isomorphic_decode(value);
        auto mime_type = MimeSniff::MimeType::parse(decoded);

        // 3. If mimeType is failure, then return false.
        if (!mime_type.has_value())
            return false;

        // 4. If mimeType’s essence is not "application/x-www-form-urlencoded", "multipart/form-data", or "text/plain", then return false.
        if (!mime_type->essence().is_one_of("application/x-www-form-urlencoded"sv, "multipart/form-data"sv, "text/plain"sv))
            return false;
    }
    // `range`
    else if (name.equals_ignoring_ascii_case("range"sv)) {
        // 1. Let rangeValue be the result of parsing a single range header value given value and false.
        auto range_value = HTTP::parse_single_range_header_value(value, false);

        // 2. If rangeValue is failure, then return false.
        if (!range_value.has_value())
            return false;

        // 3. If rangeValue[0] is null, then return false.
        // NOTE: As web browsers have historically not emitted ranges such as `bytes=-500` this algorithm does not safelist them.
        if (!range_value->start.has_value())
            return false;
    }
    // Otherwise
    else {
        // Return false.
        return false;
    }

    // 3. Return true.
    return true;
}

// https://fetch.spec.whatwg.org/#cors-unsafe-request-header-byte
bool is_cors_unsafe_request_header_byte(u8 byte)
{
    // A CORS-unsafe request-header byte is a byte byte for which one of the following is true:
    // - byte is less than 0x20 and is not 0x09 HT
    // - byte is 0x22 ("), 0x28 (left parenthesis), 0x29 (right parenthesis), 0x3A (:), 0x3C (<), 0x3E (>), 0x3F (?), 0x40 (@), 0x5B ([), 0x5C (\), 0x5D (]), 0x7B ({), 0x7D (}), or 0x7F DEL.
    return (byte < 0x20 && byte != 0x09)
        || (Array { 0x22, 0x28, 0x29, 0x3A, 0x3C, 0x3E, 0x3F, 0x40, 0x5B, 0x5C, 0x5D, 0x7B, 0x7D, 0x7F }.contains_slow(byte));
}

// https://fetch.spec.whatwg.org/#cors-unsafe-request-header-names
Vector<ByteString> get_cors_unsafe_header_names(HTTP::HeaderList const& headers)
{
    // 1. Let unsafeNames be a new list.
    Vector<ByteString> unsafe_names;

    // 2. Let potentiallyUnsafeNames be a new list.
    Vector<ByteString> potentially_unsafe_names;

    // 3. Let safelistValueSize be 0.
    Checked<size_t> safelist_value_size = 0;

    // 4. For each header of headers:
    for (auto const& header : headers) {
        // 1. If header is not a CORS-safelisted request-header, then append header’s name to unsafeNames.
        if (!is_cors_safelisted_request_header(header)) {
            unsafe_names.append(header.name);
        }
        // 2. Otherwise, append header’s name to potentiallyUnsafeNames and increase safelistValueSize by header’s
        //    value’s length.
        else {
            potentially_unsafe_names.append(header.name);
            safelist_value_size += header.value.length();
        }
    }

    // 5. If safelistValueSize is greater than 1024, then for each name of potentiallyUnsafeNames, append name to
    //    unsafeNames.
    if (safelist_value_size.has_overflow() || safelist_value_size.value() > 1024)
        unsafe_names.extend(move(potentially_unsafe_names));

    // 6. Return the result of convert header names to a sorted-lowercase set with unsafeNames.
    return HTTP::convert_header_names_to_a_sorted_lowercase_set(unsafe_names.span());
}

// https://fetch.spec.whatwg.org/#cors-non-wildcard-request-header-name
bool is_cors_non_wildcard_request_header_name(StringView header_name)
{
    // A CORS non-wildcard request-header name is a header name that is a byte-case-insensitive match for `Authorization`.
    return header_name.equals_ignoring_ascii_case("Authorization"sv);
}

// https://fetch.spec.whatwg.org/#privileged-no-cors-request-header-name
bool is_privileged_no_cors_request_header_name(StringView header_name)
{
    // A privileged no-CORS request-header name is a header name that is a byte-case-insensitive match for one of
    // - `Range`.
    return header_name.equals_ignoring_ascii_case("Range"sv);
}

// https://fetch.spec.whatwg.org/#cors-safelisted-response-header-name
bool is_cors_safelisted_response_header_name(StringView header_name, ReadonlySpan<StringView> list)
{
    // A CORS-safelisted response-header name, given a list of header names list, is a header name that is a byte-case-insensitive match for one of
    // - `Cache-Control`
    // - `Content-Language`
    // - `Content-Length`
    // - `Content-Type`
    // - `Expires`
    // - `Last-Modified`
    // - `Pragma`
    // - Any item in list that is not a forbidden response-header name.
    return header_name.is_one_of_ignoring_ascii_case(
               "Cache-Control"sv,
               "Content-Language"sv,
               "Content-Length"sv,
               "Content-Type"sv,
               "Expires"sv,
               "Last-Modified"sv,
               "Pragma"sv)
        || any_of(list, [&](auto list_header_name) {
               return header_name.equals_ignoring_ascii_case(list_header_name)
                   && !HTTP::is_forbidden_response_header_name(list_header_name);
           });
}

// https://fetch.spec.whatwg.org/#no-cors-safelisted-request-header-name
bool is_no_cors_safelisted_request_header_name(StringView header_name)
{
    // A no-CORS-safelisted request-header name is a header name that is a byte-case-insensitive match for one of
    // - `Accept`
    // - `Accept-Language`
    // - `Content-Language`
    // - `Content-Type`
    return header_name.is_one_of_ignoring_ascii_case(
        "Accept"sv,
        "Accept-Language"sv,
        "Content-Language"sv,
        "Content-Type"sv);
}

// https://fetch.spec.whatwg.org/#no-cors-safelisted-request-header
bool is_no_cors_safelisted_request_header(HTTP::Header const& header)
{
    // 1. If name is not a no-CORS-safelisted request-header name, then return false.
    if (!is_no_cors_safelisted_request_header_name(header.name))
        return false;

    // 2. Return whether (name, value) is a CORS-safelisted request-header.
    return is_cors_safelisted_request_header(header);
}

}
