/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/StringUtils.h>
#include <LibHTTP/HeaderList.h>

namespace HTTP {

NonnullRefPtr<HeaderList> HeaderList::create(Vector<Header> headers)
{
    return adopt_ref(*new HeaderList { move(headers) });
}

HeaderList::HeaderList(Vector<Header> headers)
    : m_headers(move(headers))
{
}

// https://fetch.spec.whatwg.org/#header-list-contains
bool HeaderList::contains(StringView name) const
{
    // A header list list contains a header name name if list contains a header whose name is a byte-case-insensitive
    // match for name.
    return any_of(m_headers, [&](auto const& header) {
        return header.name.equals_ignoring_ascii_case(name);
    });
}

// https://fetch.spec.whatwg.org/#concept-header-list-get
Optional<ByteString> HeaderList::get(StringView name) const
{
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
    // 1. If list contains name, then set name to the first such header’s name.
    // NOTE: This reuses the casing of the name of the header already in list, if any. If there are multiple matched
    //       headers their names will all be identical.
    auto matching_header = m_headers.first_matching([&](auto const& existing_header) {
        return existing_header.name.equals_ignoring_ascii_case(header.name);
    });

    if (matching_header.has_value())
        header.name = matching_header->name;

    // 2. Append (name, value) to list.
    m_headers.append(move(header));
}

// https://fetch.spec.whatwg.org/#concept-header-list-delete
void HeaderList::delete_(StringView name)
{
    // To delete a header name name from a header list list, remove all headers whose name is a byte-case-insensitive
    // match for name from list.
    m_headers.remove_all_matching([&](auto const& header) {
        return header.name.equals_ignoring_ascii_case(name);
    });
}

// https://fetch.spec.whatwg.org/#concept-header-list-set
void HeaderList::set(Header header)
{
    // 1. If list contains name, then set the value of the first such header to value and remove the others.
    auto it = m_headers.find_if([&](auto const& existing_header) {
        return existing_header.name.equals_ignoring_ascii_case(header.name);
    });

    if (it != m_headers.end()) {
        it->value = move(header.value);

        size_t i = 0;
        m_headers.remove_all_matching([&](auto const& existing_header) {
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
    // 1. If list contains name, then set the value of the first such header to its value, followed by 0x2C 0x20,
    //    followed by value.
    auto matching_header = m_headers.first_matching([&](auto const& existing_header) {
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
    // 1. Let headers be an empty list of headers with the key being the name and value the value.
    Vector<Header> headers;

    // 2. Let names be the result of convert header names to a sorted-lowercase set with all the names of the headers
    //    in list.
    Vector<ByteString> names_list;
    names_list.ensure_capacity(m_headers.size());

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
                    headers.empend(name, value);
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
        return Empty {};

    // FIXME: 2. If the ABNF for name allows a single header and list contains more than one, then return failure.
    // NOTE: If different error handling is needed, extract the desired header first.

    // 3. Let values be an empty list.
    Vector<ByteString> values;

    // 4. For each header header list contains whose name is name:
    for (auto const& header : m_headers) {
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

// https://wicg.github.io/background-fetch/#single-byte-content-range
static Optional<HeaderList::ContentRangeValues> parse_single_byte_content_range_as_values(ByteString const& string)
{
    HeaderList::ContentRangeValues result;
    GenericLexer lexer { string };

    // "bytes=" first-byte-pos "-" last-byte-pos "/" complete-length

    // AD-HOC: The spec wants an '=', but the RFC mentioned in the spec requires a space.
    //         https://github.com/WICG/background-fetch/issues/154
    if (!lexer.consume_specific("bytes "sv))
        return {};

    // first-byte-pos = 1*DIGIT
    auto first_byte_pos_result = lexer.consume_decimal_integer<u64>();
    if (first_byte_pos_result.is_error())
        return {};
    result.first_byte_pos = first_byte_pos_result.release_value();

    if (!lexer.consume_specific('-'))
        return {};

    // last-byte-pos  = 1*DIGIT
    auto last_byte_pos_result = lexer.consume_decimal_integer<u64>();
    if (last_byte_pos_result.is_error())
        return {};
    result.last_byte_pos = last_byte_pos_result.release_value();

    if (!lexer.consume_specific('/'))
        return {};

    // complete-length = ( 1*DIGIT / "*" )
    if (lexer.consume_specific('*')) {
        result.complete_length = {};
    } else {
        auto complete_length_result = lexer.consume_decimal_integer<u64>();
        if (complete_length_result.is_error())
            return {};
        result.complete_length = complete_length_result.release_value();
    }

    if (!lexer.is_eof())
        return {};

    return result;
}

// https://wicg.github.io/background-fetch/#extract-content-range-values
Variant<HeaderList::ContentRangeValues, HeaderList::ExtractContentRangeFailure> HeaderList::extract_content_range_values() const
{
    // 1. If response’s header list does not contain `Content-Range`, then return failure.
    // 2. Let contentRangeValue be the value of the first header whose name is a byte-case-insensitive match for
    //    `Content-Range` in response’s header list.
    auto content_range_value = get("Content-Range"sv);
    if (!content_range_value.has_value())
        return ExtractContentRangeFailure {};

    // 3. If parsing contentRangeValue per single byte content-range fails, then return failure.
    // 4. Let firstBytePos be the portion of contentRangeValue named first-byte-pos when parsed as single byte content-range,
    //    parsed as an integer.
    // 5. Let lastBytePos be the portion of contentRangeValue named last-byte-pos when parsed as single byte content-range,
    //    parsed as an integer.
    // 6. Let completeLength be the portion of contentRangeValue named complete-length when parsed as single byte
    //    content-range.
    // 7. If completeLength is "*", then set completeLength to null, otherwise set completeLength to completeLength parsed as
    //    an integer.

    // NB: The variables above are converted to integers as part of the single byte content-range parsing algorithm.
    auto result = parse_single_byte_content_range_as_values(content_range_value.value());
    if (!result.has_value())
        return ExtractContentRangeFailure {};

    // 8. Return firstBytePos, lastBytePos, and completeLength.
    return result.release_value();
}

// Non-standard
Vector<ByteString> HeaderList::unique_names() const
{
    HashTable<StringView, CaseInsensitiveASCIIStringTraits> header_names_seen;
    Vector<ByteString> header_names;

    for (auto const& header : m_headers) {
        if (header_names_seen.contains(header.name))
            continue;

        header_names_seen.set(header.name);
        header_names.append(header.name);
    }

    return header_names;
}

}
