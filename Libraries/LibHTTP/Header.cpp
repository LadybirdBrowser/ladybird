/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/QuickSort.h>
#include <LibHTTP/HTTP.h>
#include <LibHTTP/Header.h>
#include <LibHTTP/Method.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibRegex/Regex.h>
#include <LibTextCodec/Decoder.h>
#include <LibTextCodec/Encoder.h>

namespace HTTP {

Header Header::isomorphic_encode(StringView name, StringView value)
{
    return { TextCodec::isomorphic_encode(name), TextCodec::isomorphic_encode(value) };
}

// https://fetch.spec.whatwg.org/#extract-header-values
Optional<Vector<ByteString>> Header::extract_header_values() const
{
    // FIXME: 1. If parsing header’s value, per the ABNF for header’s name, fails, then return failure.
    // FIXME: 2. Return one or more values resulting from parsing header’s value, per the ABNF for header’s name.

    // For now we only parse some headers that are of the ABNF list form "#something"
    if (name.is_one_of_ignoring_ascii_case(
            "Accept-Ranges"sv,
            "Access-Control-Request-Headers"sv,
            "Access-Control-Expose-Headers"sv,
            "Access-Control-Allow-Headers"sv,
            "Access-Control-Allow-Methods"sv)
        && !value.is_empty()) {
        Vector<ByteString> trimmed_values;

        value.view().for_each_split_view(',', SplitBehavior::Nothing, [&](auto value) {
            trimmed_values.append(value.trim(" \t"sv));
        });

        return trimmed_values;
    }

    // This always ignores the ABNF rules for now and returns the header value as a single list item.
    return Vector { value };
}

// https://fetch.spec.whatwg.org/#header-name
bool is_header_name(StringView header_name)
{
    // A header name is a byte sequence that matches the field-name token production.
    Regex<ECMA262Parser> regex { R"~~~(^[A-Za-z0-9!#$%&'*+\-.^_`|~]+$)~~~" };
    return regex.has_match(header_name);
}

// https://fetch.spec.whatwg.org/#header-value
bool is_header_value(StringView header_value)
{
    // A header value is a byte sequence that matches the following conditions:
    // - Has no leading or trailing HTTP tab or space bytes.
    // - Contains no 0x00 (NUL) or HTTP newline bytes.
    if (header_value.is_empty())
        return true;

    auto first_byte = header_value[0];
    auto last_byte = header_value[header_value.length() - 1];

    if (is_http_tab_or_space(first_byte) || is_http_tab_or_space(last_byte))
        return false;

    return !any_of(header_value, [](auto byte) {
        return byte == 0x00 || is_http_newline(byte);
    });
}

// https://fetch.spec.whatwg.org/#concept-header-value-normalize
ByteString normalize_header_value(StringView potential_value)
{
    // To normalize a byte sequence potentialValue, remove any leading and trailing HTTP whitespace bytes from
    // potentialValue.
    if (potential_value.is_empty())
        return {};
    return potential_value.trim(HTTP_WHITESPACE, TrimMode::Both);
}

// https://fetch.spec.whatwg.org/#forbidden-header-name
bool is_forbidden_request_header(Header const& header)
{
    auto const& [name, value] = header;

    // 1. If name is a byte-case-insensitive match for one of:
    // [...]
    // then return true.
    if (name.is_one_of_ignoring_ascii_case(
            "Accept-Charset"sv,
            "Accept-Encoding"sv,
            "Access-Control-Request-Headers"sv,
            "Access-Control-Request-Method"sv,
            "Connection"sv,
            "Content-Length"sv,
            "Cookie"sv,
            "Cookie2"sv,
            "Date"sv,
            "DNT"sv,
            "Expect"sv,
            "Host"sv,
            "Keep-Alive"sv,
            "Origin"sv,
            "Referer"sv,
            "Set-Cookie"sv,
            "TE"sv,
            "Trailer"sv,
            "Transfer-Encoding"sv,
            "Upgrade"sv,
            "Via"sv)) {
        return true;
    }

    // 2. If name when byte-lowercased starts with `proxy-` or `sec-`, then return true.
    if (name.starts_with("proxy-"sv, CaseSensitivity::CaseInsensitive)
        || name.starts_with("sec-"sv, CaseSensitivity::CaseInsensitive)) {
        return true;
    }

    // 3. If name is a byte-case-insensitive match for one of:
    // - `X-HTTP-Method`
    // - `X-HTTP-Method-Override`
    // - `X-Method-Override`
    // then:
    if (name.is_one_of_ignoring_ascii_case(
            "X-HTTP-Method"sv,
            "X-HTTP-Method-Override"sv,
            "X-Method-Override"sv)) {
        // 1. Let parsedValues be the result of getting, decoding, and splitting value.
        auto parsed_values = get_decode_and_split_header_value(value);

        // 2. For each method of parsedValues: if the isomorphic encoding of method is a forbidden method, then return true.
        // NB: The values returned from get_decode_and_split_header_value have already been decoded.
        if (any_of(parsed_values, [](auto const& method) { return is_forbidden_method(method); }))
            return true;
    }

    // 4. Return false.
    return false;
}

// https://fetch.spec.whatwg.org/#forbidden-response-header-name
bool is_forbidden_response_header_name(StringView header_name)
{
    // A forbidden response-header name is a header name that is a byte-case-insensitive match for one of:
    // - `Set-Cookie`
    // - `Set-Cookie2`
    return header_name.is_one_of_ignoring_ascii_case(
        "Set-Cookie"sv,
        "Set-Cookie2"sv);
}

// https://fetch.spec.whatwg.org/#header-value-get-decode-and-split
Vector<String> get_decode_and_split_header_value(StringView value)
{
    // 1. Let input be the result of isomorphic decoding value.
    auto input = TextCodec::isomorphic_decode(value);

    // 2. Let position be a position variable for input, initially pointing at the start of input.
    GenericLexer lexer { input };

    // 3. Let values be a list of strings, initially « ».
    Vector<String> values;

    // 4. Let temporaryValue be the empty string.
    StringBuilder temporary_value_builder;

    // 5. While true:
    while (true) {
        // 1. Append the result of collecting a sequence of code points that are not U+0022 (") or U+002C (,) from
        //    input, given position, to temporaryValue.
        // NOTE: The result might be the empty string.
        temporary_value_builder.append(lexer.consume_until(is_any_of("\","sv)));

        // 2. If position is not past the end of input and the code point at position within input is U+0022 ("):
        if (!lexer.is_eof() && lexer.peek() == '"') {
            // 1. Append the result of collecting an HTTP quoted string from input, given position, to temporaryValue.
            temporary_value_builder.append(collect_an_http_quoted_string(lexer));

            // 2. If position is not past the end of input, then continue.
            if (!lexer.is_eof())
                continue;
        }

        // 3. Remove all HTTP tab or space from the start and end of temporaryValue.
        auto temporary_value = MUST(String::from_utf8(temporary_value_builder.string_view().trim(HTTP_TAB_OR_SPACE, TrimMode::Both)));

        // 4. Append temporaryValue to values.
        values.append(move(temporary_value));

        // 5. Set temporaryValue to the empty string.
        temporary_value_builder.clear();

        // 6. If position is past the end of input, then return values.
        if (lexer.is_eof())
            return values;

        // 7. Assert: the code point at position within input is U+002C (,).
        VERIFY(lexer.peek() == ',');

        // 8. Advance position by 1.
        lexer.ignore(1);
    }
}

// https://fetch.spec.whatwg.org/#convert-header-names-to-a-sorted-lowercase-set
Vector<ByteString> convert_header_names_to_a_sorted_lowercase_set(ReadonlySpan<ByteString> header_names)
{
    // 1. Let headerNamesSet be a new ordered set.
    HashTable<StringView, CaseInsensitiveASCIIStringTraits> header_names_seen;
    Vector<ByteString> header_names_set;

    // 2. For each name of headerNames, append the result of byte-lowercasing name to headerNamesSet.
    for (auto const& name : header_names) {
        if (header_names_seen.contains(name))
            continue;

        header_names_seen.set(name);
        header_names_set.append(name.to_lowercase());
    }

    // 3. Return the result of sorting headerNamesSet in ascending order with byte less than.
    quick_sort(header_names_set);
    return header_names_set;
}

// https://fetch.spec.whatwg.org/#build-a-content-range
ByteString build_content_range(u64 range_start, u64 range_end, u64 full_length)
{
    // 1. Let contentRange be `bytes `.
    // 2. Append rangeStart, serialized and isomorphic encoded, to contentRange.
    // 3. Append 0x2D (-) to contentRange.
    // 4. Append rangeEnd, serialized and isomorphic encoded to contentRange.
    // 5. Append 0x2F (/) to contentRange.
    // 6. Append fullLength, serialized and isomorphic encoded to contentRange.
    // 7. Return contentRange.
    return ByteString::formatted("bytes {}-{}/{}", range_start, range_end, full_length);
}

// https://fetch.spec.whatwg.org/#simple-range-header-value
Optional<RangeHeaderValue> parse_single_range_header_value(StringView value, bool allow_whitespace)
{
    // 1. Let data be the isomorphic decoding of value.
    auto data = TextCodec::isomorphic_decode(value);

    // 2. If data does not start with "bytes", then return failure.
    if (!data.starts_with_bytes("bytes"sv))
        return {};

    // 3. Let position be a position variable for data, initially pointing at the 5th code point of data.
    GenericLexer lexer { data };
    lexer.ignore(5);

    // 4. If allowWhitespace is true, collect a sequence of code points that are HTTP tab or space, from data given position.
    if (allow_whitespace)
        lexer.consume_while(is_http_tab_or_space);

    // 5. If the code point at position within data is not U+003D (=), then return failure.
    // 6. Advance position by 1.
    if (!lexer.consume_specific('='))
        return {};

    // 7. If allowWhitespace is true, collect a sequence of code points that are HTTP tab or space, from data given position.
    if (allow_whitespace)
        lexer.consume_while(is_http_tab_or_space);

    // 8. Let rangeStart be the result of collecting a sequence of code points that are ASCII digits, from data given position.
    auto range_start = lexer.consume_while(is_ascii_digit);

    // 9. Let rangeStartValue be rangeStart, interpreted as decimal number, if rangeStart is not the empty string;
    //    otherwise null.
    auto range_start_value = range_start.to_number<u64>();

    // 10. If allowWhitespace is true, collect a sequence of code points that are HTTP tab or space, from data given position.
    if (allow_whitespace)
        lexer.consume_while(is_http_tab_or_space);

    // 11. If the code point at position within data is not U+002D (-), then return failure.
    // 12. Advance position by 1.
    if (!lexer.consume_specific('-'))
        return {};

    // 13. If allowWhitespace is true, collect a sequence of code points that are HTTP tab or space, from data given position.
    if (allow_whitespace)
        lexer.consume_while(is_http_tab_or_space);

    // 14. Let rangeEnd be the result of collecting a sequence of code points that are ASCII digits, from data given position.
    auto range_end = lexer.consume_while(is_ascii_digit);

    // 15. Let rangeEndValue be rangeEnd, interpreted as decimal number, if rangeEnd is not the empty string; otherwise null.
    auto range_end_value = range_end.to_number<u64>();

    // 16. If position is not past the end of data, then return failure.
    if (!lexer.is_eof())
        return {};

    // 17. If rangeEndValue and rangeStartValue are null, then return failure.
    if (!range_end_value.has_value() && !range_start_value.has_value())
        return {};

    // 18. If rangeStartValue and rangeEndValue are numbers, and rangeStartValue is greater than rangeEndValue, then
    //     return failure.
    if (range_start_value.has_value() && range_end_value.has_value() && *range_start_value > *range_end_value)
        return {};

    // 19. Return (rangeStartValue, rangeEndValue).
    return RangeHeaderValue { range_start_value, range_end_value };
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, HTTP::Header const& header)
{
    TRY(encoder.encode(header.name));
    TRY(encoder.encode(header.value));
    return {};
}

template<>
ErrorOr<HTTP::Header> decode(Decoder& decoder)
{
    auto name = TRY(decoder.decode<ByteString>());
    auto value = TRY(decoder.decode<ByteString>());
    return HTTP::Header { move(name), move(value) };
}

}
