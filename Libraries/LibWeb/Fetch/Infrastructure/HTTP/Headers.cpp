/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Checked.h>
#include <AK/GenericLexer.h>
#include <AK/QuickSort.h>
#include <AK/StringUtils.h>
#include <LibJS/Runtime/VM.h>
#include <LibRegex/Regex.h>
#include <LibTextCodec/Decoder.h>
#include <LibTextCodec/Encoder.h>
#include <LibWeb/Fetch/Infrastructure/HTTP.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Headers.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Methods.h>
#include <LibWeb/Loader/ResourceLoader.h>

namespace Web::Fetch::Infrastructure {

GC_DEFINE_ALLOCATOR(HeaderList);

Header Header::isomorphic_encode(StringView name, StringView value)
{
    return {
        .name = TextCodec::isomorphic_encode(name),
        .value = TextCodec::isomorphic_encode(value),
    };
}

// https://fetch.spec.whatwg.org/#extract-header-values
Optional<Vector<ByteString>> Header::extract_header_values() const
{
    // FIXME: 1. If parsing header’s value, per the ABNF for header’s name, fails, then return failure.
    // FIXME: 2. Return one or more values resulting from parsing header’s value, per the ABNF for header’s name.

    // For now we only parse some headers that are of the ABNF list form "#something"
    if (name.is_one_of_ignoring_ascii_case(
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

GC::Ref<HeaderList> HeaderList::create(JS::VM& vm)
{
    return vm.heap().allocate<HeaderList>();
}

// https://fetch.spec.whatwg.org/#header-list-contains
bool HeaderList::contains(StringView name) const
{
    // A header list list contains a header name name if list contains a header whose name is a byte-case-insensitive
    // match for name.
    return any_of(*this, [&](auto const& header) {
        return header.name.equals_ignoring_ascii_case(name);
    });
}

// https://fetch.spec.whatwg.org/#concept-header-list-get
Optional<ByteString> HeaderList::get(StringView name) const
{
    // To get a header name name from a header list list, run these steps:

    // 1. If list does not contain name, then return null.
    if (!contains(name))
        return {};

    // 2. Return the values of all headers in list whose name is a byte-case-insensitive match for name, separated from
    //    each other by 0x2C 0x20, in order.
    StringBuilder builder;
    bool first = true;

    for (auto const& header : *this) {
        if (!header.name.equals_ignoring_ascii_case(name))
            continue;

        if (!first)
            builder.append(", "sv);

        builder.append(header.value);
        first = false;
    }

    return builder.to_byte_string();
}

// https://fetch.spec.whatwg.org/#concept-header-list-get-decode-split
Optional<Vector<String>> HeaderList::get_decode_and_split(StringView name) const
{
    // To get, decode, and split a header name name from header list list, run these steps:

    // 1. Let value be the result of getting name from list.
    auto value = get(name);

    // 2. If value is null, then return null.
    if (!value.has_value())
        return {};

    // 3. Return the result of getting, decoding, and splitting value.
    return get_decode_and_split_header_value(*value);
}

// https://fetch.spec.whatwg.org/#concept-header-list-append
void HeaderList::append(Header header)
{
    // To append a header (name, value) to a header list list, run these steps:

    // 1. If list contains name, then set name to the first such header’s name.
    // NOTE: This reuses the casing of the name of the header already in list, if any. If there are multiple matched
    //       headers their names will all be identical.
    auto matching_header = first_matching([&](auto const& existing_header) {
        return existing_header.name.equals_ignoring_ascii_case(header.name);
    });

    if (matching_header.has_value())
        header.name = matching_header->name;

    // 2. Append (name, value) to list.
    Vector::append(move(header));
}

// https://fetch.spec.whatwg.org/#concept-header-list-delete
void HeaderList::delete_(StringView name)
{
    // To delete a header name name from a header list list, remove all headers whose name is a byte-case-insensitive
    // match for name from list.
    remove_all_matching([&](auto const& header) {
        return header.name.equals_ignoring_ascii_case(name);
    });
}

// https://fetch.spec.whatwg.org/#concept-header-list-set
void HeaderList::set(Header header)
{
    // To set a header (name, value) in a header list list, run these steps:

    // 1. If list contains name, then set the value of the first such header to value and remove the others.
    auto it = find_if([&](auto const& existing_header) {
        return existing_header.name.equals_ignoring_ascii_case(header.name);
    });

    if (it != end()) {
        it->value = move(header.value);

        size_t i = 0;
        remove_all_matching([&](auto const& existing_header) {
            if (i++ <= it.index())
                return false;

            return existing_header.name.equals_ignoring_ascii_case(it->name);
        });
    }
    // 2. Otherwise, append header (name, value) to list.
    else {
        append(move(header));
    }
}

// https://fetch.spec.whatwg.org/#concept-header-list-combine
void HeaderList::combine(Header header)
{
    // To combine a header (name, value) in a header list list, run these steps:

    // 1. If list contains name, then set the value of the first such header to its value, followed by 0x2C 0x20,
    //    followed by value.
    auto matching_header = first_matching([&](auto const& existing_header) {
        return existing_header.name.equals_ignoring_ascii_case(header.name);
    });

    if (matching_header.has_value()) {
        matching_header->value = ByteString::formatted("{}, {}", matching_header->value, header.value);
    }
    // 2. Otherwise, append (name, value) to list.
    else {
        append(move(header));
    }
}

// https://fetch.spec.whatwg.org/#concept-header-list-sort-and-combine
Vector<Header> HeaderList::sort_and_combine() const
{
    // To sort and combine a header list list, run these steps:

    // 1. Let headers be an empty list of headers with the key being the name and value the value.
    Vector<Header> headers;

    // 2. Let names be the result of convert header names to a sorted-lowercase set with all the names of the headers
    //    in list.
    Vector<ByteString> names_list;
    names_list.ensure_capacity(size());

    for (auto const& header : *this)
        names_list.unchecked_append(header.name);

    auto names = convert_header_names_to_a_sorted_lowercase_set(names_list);

    // 3. For each name of names:
    for (auto& name : names) {
        // 1. If name is `set-cookie`, then:
        if (name == "set-cookie"sv) {
            // 1. Let values be a list of all values of headers in list whose name is a byte-case-insensitive match for
            //    name, in order.
            // 2. For each value of values:
            for (auto const& [header_name, value] : *this) {
                if (header_name.equals_ignoring_ascii_case(name)) {
                    // 1. Append (name, value) to headers.
                    headers.append({ name, value });
                }
            }
        }
        // 2. Otherwise:
        else {
            // 1. Let value be the result of getting name from list.
            auto value = get(name);

            // 2. Assert: value is not null.
            VERIFY(value.has_value());

            // 3. Append (name, value) to headers.
            headers.empend(move(name), value.release_value());
        }
    }

    // 4. Return headers.
    return headers;
}

// https://fetch.spec.whatwg.org/#extract-header-list-values
Variant<Empty, Vector<ByteString>, HeaderList::ExtractHeaderParseFailure> HeaderList::extract_header_list_values(StringView name) const
{
    // 1. If list does not contain name, then return null.
    if (!contains(name))
        return {};

    // FIXME: 2. If the ABNF for name allows a single header and list contains more than one, then return failure.
    // NOTE: If different error handling is needed, extract the desired header first.

    // 3. Let values be an empty list.
    Vector<ByteString> values;

    // 4. For each header header list contains whose name is name:
    for (auto const& header : *this) {
        if (!header.name.equals_ignoring_ascii_case(name))
            continue;

        // 1. Let extract be the result of extracting header values from header.
        auto extract = header.extract_header_values();

        // 2. If extract is failure, then return failure.
        if (!extract.has_value())
            return ExtractHeaderParseFailure {};

        // 3. Append each value in extract, in order, to values.
        values.extend(extract.release_value());
    }

    // 5. Return values.
    return values;
}

// https://fetch.spec.whatwg.org/#header-list-extract-a-length
Variant<Empty, u64, HeaderList::ExtractLengthFailure> HeaderList::extract_length() const
{
    // 1. Let values be the result of getting, decoding, and splitting `Content-Length` from headers.
    auto values = get_decode_and_split("Content-Length"sv);

    // 2. If values is null, then return null.
    if (!values.has_value())
        return {};

    // 3. Let candidateValue be null.
    Optional<String> candidate_value;

    // 4. For each value of values:
    for (auto const& value : *values) {
        // 1. If candidateValue is null, then set candidateValue to value.
        if (!candidate_value.has_value()) {
            candidate_value = value;
        }
        // 2. Otherwise, if value is not candidateValue, return failure.
        else if (candidate_value.value() != value) {
            return ExtractLengthFailure {};
        }
    }

    // 5. If candidateValue is the empty string or has a code point that is not an ASCII digit, then return null.
    // 6. Return candidateValue, interpreted as decimal number.
    // FIXME: This will return an empty Optional if it cannot fit into a u64, is this correct?
    auto result = candidate_value->to_number<u64>(TrimWhitespace::No);
    if (!result.has_value())
        return {};

    return *result;
}

// Non-standard
Vector<ByteString> HeaderList::unique_names() const
{
    Vector<ByteString> header_names_set;
    HashTable<StringView, CaseInsensitiveStringTraits> header_names_seen;

    for (auto const& header : *this) {
        if (header_names_seen.contains(header.name))
            continue;

        header_names_set.append(header.name);
        header_names_seen.set(header.name);
    }

    return header_names_set;
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
    // A header (name, value) is forbidden request-header if these steps return true:
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
    // To get, decode, and split a header value value, run these steps:

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
    // To convert header names to a sorted-lowercase set, given a list of names headerNames, run these steps:

    // 1. Let headerNamesSet be a new ordered set.
    HashTable<StringView, CaseInsensitiveStringTraits> header_names_seen;
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
Optional<RangeHeaderValue> parse_single_range_header_value(StringView const value, bool const allow_whitespace)
{
    // 1. Let data be the isomorphic decoding of value.
    auto const data = TextCodec::isomorphic_decode(value);

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

    // 9. Let rangeStartValue be rangeStart, interpreted as decimal number, if rangeStart is not the empty string; otherwise null.
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

    // 18. If rangeStartValue and rangeEndValue are numbers, and rangeStartValue is greater than rangeEndValue, then return failure.
    if (range_start_value.has_value() && range_end_value.has_value() && *range_start_value > *range_end_value)
        return {};

    // 19. Return (rangeStartValue, rangeEndValue).
    return RangeHeaderValue { move(range_start_value), move(range_end_value) };
}

// https://fetch.spec.whatwg.org/#default-user-agent-value
ByteString const& default_user_agent_value()
{
    // A default `User-Agent` value is an implementation-defined header value for the `User-Agent` header.
    static auto user_agent = ResourceLoader::the().user_agent().to_byte_string();
    return user_agent;
}

}
