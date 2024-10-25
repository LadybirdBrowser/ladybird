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
#include <AK/ScopeGuard.h>
#include <AK/StringUtils.h>
#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibRegex/Regex.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Fetch/Infrastructure/HTTP.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Headers.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Methods.h>
#include <LibWeb/Infra/ByteSequences.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::Fetch::Infrastructure {

JS_DEFINE_ALLOCATOR(HeaderList);

template<typename T>
requires(IsSameIgnoringCV<T, u8>) struct CaseInsensitiveBytesTraits : public Traits<Span<T>> {
    static constexpr bool equals(Span<T> const& a, Span<T> const& b)
    {
        return StringView { a }.equals_ignoring_ascii_case(StringView { b });
    }

    static constexpr unsigned hash(Span<T> const& span)
    {
        if (span.is_empty())
            return 0;
        return AK::case_insensitive_string_hash(reinterpret_cast<char const*>(span.data()), span.size());
    }
};

Header Header::copy(Header const& header)
{
    return Header {
        .name = MUST(ByteBuffer::copy(header.name)),
        .value = MUST(ByteBuffer::copy(header.value)),
    };
}
Header Header::from_string_pair(StringView name, StringView value)
{
    return Header {
        .name = MUST(ByteBuffer::copy(name.bytes())),
        .value = MUST(ByteBuffer::copy(value.bytes())),
    };
}

JS::NonnullGCPtr<HeaderList> HeaderList::create(JS::VM& vm)
{
    return vm.heap().allocate_without_realm<HeaderList>();
}

// Non-standard
Vector<ByteBuffer> HeaderList::unique_names() const
{
    Vector<ByteBuffer> header_names_set;
    HashTable<ReadonlyBytes, CaseInsensitiveBytesTraits<u8 const>> header_names_seen;

    for (auto const& header : *this) {
        if (header_names_seen.contains(header.name))
            continue;
        header_names_seen.set(header.name);
        header_names_set.append(MUST(ByteBuffer::copy(header.name)));
    }

    return header_names_set;
}

// https://fetch.spec.whatwg.org/#header-list-contains
bool HeaderList::contains(ReadonlyBytes name) const
{
    // A header list list contains a header name name if list contains a header whose name is a byte-case-insensitive match for name.
    return any_of(*this, [&](auto const& header) {
        return StringView { header.name }.equals_ignoring_ascii_case(name);
    });
}

// https://fetch.spec.whatwg.org/#concept-header-list-get
Optional<ByteBuffer> HeaderList::get(ReadonlyBytes name) const
{
    // To get a header name name from a header list list, run these steps:

    // 1. If list does not contain name, then return null.
    if (!contains(name))
        return {};

    // 2. Return the values of all headers in list whose name is a byte-case-insensitive match for name, separated from each other by 0x2C 0x20, in order.
    ByteBuffer buffer;
    auto first = true;
    for (auto const& header : *this) {
        if (!StringView { header.name }.equals_ignoring_ascii_case(name))
            continue;
        if (first) {
            first = false;
        } else {
            buffer.append(0x2c);
            buffer.append(0x20);
        }
        buffer.append(header.value);
    }
    return buffer;
}

// https://fetch.spec.whatwg.org/#concept-header-list-get-decode-split
Optional<Vector<String>> HeaderList::get_decode_and_split(ReadonlyBytes name) const
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

// https://fetch.spec.whatwg.org/#header-value-get-decode-and-split
Optional<Vector<String>> get_decode_and_split_header_value(ReadonlyBytes value)
{
    // To get, decode, and split a header value value, run these steps:

    // 1. Let input be the result of isomorphic decoding value.
    auto input = StringView { value };

    // 2. Let position be a position variable for input, initially pointing at the start of input.
    auto lexer = GenericLexer { input };

    // 3. Let values be a list of strings, initially « ».
    Vector<String> values;

    // 4. Let temporaryValue be the empty string.
    StringBuilder temporary_value_builder;

    // 5. While true:
    while (true) {
        // 1. Append the result of collecting a sequence of code points that are not U+0022 (") or U+002C (,) from input, given position, to temporaryValue.
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

// https://fetch.spec.whatwg.org/#concept-header-list-append
void HeaderList::append(Header header)
{
    // To append a header (name, value) to a header list list, run these steps:
    // NOTE: Can't use structured bindings captured in the lambda due to https://github.com/llvm/llvm-project/issues/48582
    auto& name = header.name;

    // 1. If list contains name, then set name to the first such header’s name.
    // NOTE: This reuses the casing of the name of the header already in list, if any. If there are multiple matched headers their names will all be identical.
    if (contains(name)) {
        auto matching_header = first_matching([&](auto const& existing_header) {
            return StringView { existing_header.name }.equals_ignoring_ascii_case(name);
        });
        name.overwrite(0, matching_header->name.data(), matching_header->name.size());
    }

    // 2. Append (name, value) to list.
    Vector<Header>::append(move(header));
}

// https://fetch.spec.whatwg.org/#concept-header-list-delete
void HeaderList::delete_(ReadonlyBytes name)
{
    // To delete a header name name from a header list list, remove all headers whose name is a byte-case-insensitive match for name from list.
    remove_all_matching([&](auto const& header) {
        return StringView { header.name }.equals_ignoring_ascii_case(name);
    });
}

// https://fetch.spec.whatwg.org/#concept-header-list-set
void HeaderList::set(Header header)
{
    // To set a header (name, value) in a header list list, run these steps:
    // NOTE: Can't use structured bindings captured in the lambda due to https://github.com/llvm/llvm-project/issues/48582
    auto const& name = header.name;
    auto const& value = header.value;

    // 1. If list contains name, then set the value of the first such header to value and remove the others.
    if (contains(name)) {
        auto matching_index = find_if([&](auto const& existing_header) {
            return StringView { existing_header.name }.equals_ignoring_ascii_case(name);
        }).index();
        auto& matching_header = at(matching_index);
        matching_header.value = MUST(ByteBuffer::copy(value));
        size_t i = 0;
        remove_all_matching([&](auto const& existing_header) {
            ScopeGuard increment_i = [&]() { i++; };
            if (i <= matching_index)
                return false;
            return StringView { existing_header.name }.equals_ignoring_ascii_case(name);
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
    // NOTE: Can't use structured bindings captured in the lambda due to https://github.com/llvm/llvm-project/issues/48582
    auto const& name = header.name;
    auto const& value = header.value;

    // 1. If list contains name, then set the value of the first such header to its value, followed by 0x2C 0x20, followed by value.
    if (contains(name)) {
        auto matching_header = first_matching([&](auto const& existing_header) {
            return StringView { existing_header.name }.equals_ignoring_ascii_case(name);
        });
        matching_header->value.append(0x2c);
        matching_header->value.append(0x20);
        matching_header->value.append(value);
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

    // 2. Let names be the result of convert header names to a sorted-lowercase set with all the names of the headers in list.
    Vector<ReadonlyBytes> names_list;
    names_list.ensure_capacity(size());
    for (auto const& header : *this)
        names_list.unchecked_append(header.name);
    auto names = convert_header_names_to_a_sorted_lowercase_set(names_list);

    // 3. For each name of names:
    for (auto& name : names) {
        // 1. If name is `set-cookie`, then:
        if (name == "set-cookie"sv.bytes()) {
            // 1. Let values be a list of all values of headers in list whose name is a byte-case-insensitive match for name, in order.
            // 2. For each value of values:
            for (auto const& [header_name, value] : *this) {
                if (StringView { header_name }.equals_ignoring_ascii_case(name)) {
                    // 1. Append (name, value) to headers.
                    auto header = Header::from_string_pair(name, value);
                    headers.append(move(header));
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
            auto header = Header {
                .name = move(name),
                .value = value.release_value(),
            };
            headers.append(move(header));
        }
    }

    // 4. Return headers.
    return headers;
}

// https://fetch.spec.whatwg.org/#header-list-extract-a-length
HeaderList::ExtractLengthResult HeaderList::extract_length() const
{
    // 1. Let values be the result of getting, decoding, and splitting `Content-Length` from headers.
    auto values = get_decode_and_split("Content-Length"sv.bytes());

    // 2. If values is null, then return null.
    if (!values.has_value())
        return Empty {};

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
    // NOTE: to_number does this for us.
    // 6. Return candidateValue, interpreted as decimal number.
    // NOTE: The spec doesn't say anything about trimming here, so we don't trim. If it contains a space, step 5 will cause us to return null.
    // FIXME: This will return an empty Optional if it cannot fit into a u64, is this correct?
    auto conversion_result = candidate_value.value().to_number<u64>(TrimWhitespace::No);
    if (!conversion_result.has_value())
        return Empty {};
    return ExtractLengthResult { conversion_result.release_value() };
}

// https://fetch.spec.whatwg.org/#concept-header-extract-mime-type
Optional<MimeSniff::MimeType> HeaderList::extract_mime_type() const
{
    // 1. Let charset be null.
    Optional<String> charset;

    // 2. Let essence be null.
    Optional<String> essence;

    // 3. Let mimeType be null.
    Optional<MimeSniff::MimeType> mime_type;

    // 4. Let values be the result of getting, decoding, and splitting `Content-Type` from headers.
    auto values = get_decode_and_split("Content-Type"sv.bytes());

    // 5. If values is null, then return failure.
    if (!values.has_value())
        return {};

    // 6. For each value of values:
    for (auto const& value : *values) {
        // 1. Let temporaryMimeType be the result of parsing value.
        auto temporary_mime_type = MimeSniff::MimeType::parse(value);

        // 2. If temporaryMimeType is failure or its essence is "*/*", then continue.
        if (!temporary_mime_type.has_value() || temporary_mime_type->essence() == "*/*"sv)
            continue;

        // 3. Set mimeType to temporaryMimeType.
        mime_type = temporary_mime_type;

        // 4. If mimeType’s essence is not essence, then:
        if (!essence.has_value() || (mime_type->essence() != essence->bytes_as_string_view())) {
            // 1. Set charset to null.
            charset = {};

            // 2. If mimeType’s parameters["charset"] exists, then set charset to mimeType’s parameters["charset"].
            auto it = mime_type->parameters().find("charset"sv);
            if (it != mime_type->parameters().end())
                charset = it->value;

            // 3. Set essence to mimeType’s essence.
            essence = mime_type->essence();
        }
        // 5. Otherwise, if mimeType’s parameters["charset"] does not exist, and charset is non-null, set mimeType’s parameters["charset"] to charset.
        else if (!mime_type->parameters().contains("charset"sv) && charset.has_value()) {
            mime_type->set_parameter("charset"_string, charset.release_value());
        }
    }

    // 7. If mimeType is null, then return failure.
    // 8. Return mimeType.
    return mime_type;
}

// https://fetch.spec.whatwg.org/#legacy-extract-an-encoding
StringView legacy_extract_an_encoding(Optional<MimeSniff::MimeType> const& mime_type, StringView fallback_encoding)
{
    // 1. If mimeType is failure, then return fallbackEncoding.
    if (!mime_type.has_value())
        return fallback_encoding;

    // 2. If mimeType["charset"] does not exist, then return fallbackEncoding.
    auto charset = mime_type->parameters().get("charset"sv);
    if (!charset.has_value())
        return fallback_encoding;

    // 3. Let tentativeEncoding be the result of getting an encoding from mimeType["charset"].
    auto tentative_encoding = TextCodec::get_standardized_encoding(*charset);

    // 4. If tentativeEncoding is failure, then return fallbackEncoding.
    if (!tentative_encoding.has_value())
        return fallback_encoding;

    // 5. Return tentativeEncoding.
    return *tentative_encoding;
}

// https://fetch.spec.whatwg.org/#convert-header-names-to-a-sorted-lowercase-set
OrderedHashTable<ByteBuffer> convert_header_names_to_a_sorted_lowercase_set(Span<ReadonlyBytes> header_names)
{
    // To convert header names to a sorted-lowercase set, given a list of names headerNames, run these steps:

    // 1. Let headerNamesSet be a new ordered set.
    Vector<ByteBuffer> header_names_set;
    HashTable<ReadonlyBytes, CaseInsensitiveBytesTraits<u8 const>> header_names_seen;

    // 2. For each name of headerNames, append the result of byte-lowercasing name to headerNamesSet.
    for (auto name : header_names) {
        if (header_names_seen.contains(name))
            continue;
        auto bytes = MUST(ByteBuffer::copy(name));
        Infra::byte_lowercase(bytes);
        header_names_seen.set(name);
        header_names_set.append(move(bytes));
    }

    // 3. Return the result of sorting headerNamesSet in ascending order with byte less than.
    quick_sort(header_names_set, [](auto const& a, auto const& b) {
        return StringView { a } < StringView { b };
    });
    OrderedHashTable<ByteBuffer> ordered { header_names_set.size() };
    for (auto& name : header_names_set) {
        auto result = ordered.set(move(name));
        VERIFY(result == AK::HashSetResult::InsertedNewEntry);
    }
    return ordered;
}

// https://fetch.spec.whatwg.org/#header-name
bool is_header_name(ReadonlyBytes header_name)
{
    // A header name is a byte sequence that matches the field-name token production.
    Regex<ECMA262Parser> regex { R"~~~(^[A-Za-z0-9!#$%&'*+\-.^_`|~]+$)~~~" };
    return regex.has_match(StringView { header_name });
}

// https://fetch.spec.whatwg.org/#header-value
bool is_header_value(ReadonlyBytes header_value)
{
    // A header value is a byte sequence that matches the following conditions:
    // - Has no leading or trailing HTTP tab or space bytes.
    // - Contains no 0x00 (NUL) or HTTP newline bytes.
    if (header_value.is_empty())
        return true;
    auto first_byte = header_value[0];
    auto last_byte = header_value[header_value.size() - 1];
    if (HTTP_TAB_OR_SPACE_BYTES.span().contains_slow(first_byte) || HTTP_TAB_OR_SPACE_BYTES.span().contains_slow(last_byte))
        return false;
    return !any_of(header_value, [](auto byte) {
        return byte == 0x00 || HTTP_NEWLINE_BYTES.span().contains_slow(byte);
    });
}

// https://fetch.spec.whatwg.org/#concept-header-value-normalize
ByteBuffer normalize_header_value(ReadonlyBytes potential_value)
{
    // To normalize a byte sequence potentialValue, remove any leading and trailing HTTP whitespace bytes from potentialValue.
    if (potential_value.is_empty())
        return {};
    auto trimmed = StringView { potential_value }.trim(HTTP_WHITESPACE, TrimMode::Both);
    return MUST(ByteBuffer::copy(trimmed.bytes()));
}

// https://fetch.spec.whatwg.org/#cors-safelisted-request-header
bool is_cors_safelisted_request_header(Header const& header)
{
    // To determine whether a header (name, value) is a CORS-safelisted request-header, run these steps:

    auto const& value = header.value;

    // 1. If value’s length is greater than 128, then return false.
    if (value.size() > 128)
        return false;

    // 2. Byte-lowercase name and switch on the result:
    auto name = StringView { header.name };

    // `accept`
    if (name.equals_ignoring_ascii_case("accept"sv)) {
        // If value contains a CORS-unsafe request-header byte, then return false.
        if (any_of(value.span(), is_cors_unsafe_request_header_byte))
            return false;
    }
    // `accept-language`
    // `content-language`
    else if (name.is_one_of_ignoring_ascii_case("accept-language"sv, "content-language"sv)) {
        // If value contains a byte that is not in the range 0x30 (0) to 0x39 (9), inclusive, is not in the range 0x41 (A) to 0x5A (Z), inclusive, is not in the range 0x61 (a) to 0x7A (z), inclusive, and is not 0x20 (SP), 0x2A (*), 0x2C (,), 0x2D (-), 0x2E (.), 0x3B (;), or 0x3D (=), then return false.
        if (any_of(value.span(), [](auto byte) {
                return !(is_ascii_digit(byte) || is_ascii_alpha(byte) || " *,-.;="sv.contains(static_cast<char>(byte)));
            }))
            return false;
    }
    // `content-type`
    else if (name.equals_ignoring_ascii_case("content-type"sv)) {
        // 1. If value contains a CORS-unsafe request-header byte, then return false.
        if (any_of(value.span(), is_cors_unsafe_request_header_byte))
            return false;

        // 2. Let mimeType be the result of parsing the result of isomorphic decoding value.
        auto mime_type = MimeSniff::MimeType::parse(StringView { value });

        // 3. If mimeType is failure, then return false.
        if (!mime_type.has_value())
            return false;

        // 4. If mimeType’s essence is not "application/x-www-form-urlencoded", "multipart/form-data", or "text/plain", then return false.
        if (!mime_type->essence().is_one_of("application/x-www-form-urlencoded"sv, "multipart/form-data"sv, "text/plain"sv))
            return false;
    }
    // `range`
    else if (name.equals_ignoring_ascii_case("range"sv)) {
        // 1. Let rangeValue be the result of parsing a single range header value given value.
        auto range_value = parse_single_range_header_value(value);

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
        || (Array<u8, 14> { 0x22, 0x28, 0x29, 0x3A, 0x3C, 0x3E, 0x3F, 0x40, 0x5B, 0x5C, 0x5D, 0x7B, 0x7D, 0x7F }.span().contains_slow(byte));
}

// https://fetch.spec.whatwg.org/#cors-unsafe-request-header-names
OrderedHashTable<ByteBuffer> get_cors_unsafe_header_names(HeaderList const& headers)
{
    // The CORS-unsafe request-header names, given a header list headers, are determined as follows:

    // 1. Let unsafeNames be a new list.
    Vector<ReadonlyBytes> unsafe_names;

    // 2. Let potentiallyUnsafeNames be a new list.
    Vector<ReadonlyBytes> potentially_unsafe_names;

    // 3. Let safelistValueSize be 0.
    Checked<size_t> safelist_value_size = 0;

    // 4. For each header of headers:
    for (auto const& header : headers) {
        // 1. If header is not a CORS-safelisted request-header, then append header’s name to unsafeNames.
        if (!is_cors_safelisted_request_header(header)) {
            unsafe_names.append(header.name.span());
        }
        // 2. Otherwise, append header’s name to potentiallyUnsafeNames and increase safelistValueSize by header’s value’s length.
        else {
            potentially_unsafe_names.append(header.name.span());
            safelist_value_size += header.value.size();
        }
    }

    // 5. If safelistValueSize is greater than 1024, then for each name of potentiallyUnsafeNames, append name to unsafeNames.
    if (safelist_value_size.has_overflow() || safelist_value_size.value() > 1024) {
        for (auto const& name : potentially_unsafe_names)
            unsafe_names.append(name);
    }

    // 6. Return the result of convert header names to a sorted-lowercase set with unsafeNames.
    return convert_header_names_to_a_sorted_lowercase_set(unsafe_names.span());
}

// https://fetch.spec.whatwg.org/#cors-non-wildcard-request-header-name
bool is_cors_non_wildcard_request_header_name(ReadonlyBytes header_name)
{
    // A CORS non-wildcard request-header name is a header name that is a byte-case-insensitive match for `Authorization`.
    return StringView { header_name }.equals_ignoring_ascii_case("Authorization"sv);
}

// https://fetch.spec.whatwg.org/#privileged-no-cors-request-header-name
bool is_privileged_no_cors_request_header_name(ReadonlyBytes header_name)
{
    // A privileged no-CORS request-header name is a header name that is a byte-case-insensitive match for one of
    // - `Range`.
    return StringView { header_name }.equals_ignoring_ascii_case("Range"sv);
}

// https://fetch.spec.whatwg.org/#cors-safelisted-response-header-name
bool is_cors_safelisted_response_header_name(ReadonlyBytes header_name, Span<ReadonlyBytes> list)
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
    return StringView { header_name }.is_one_of_ignoring_ascii_case(
               "Cache-Control"sv,
               "Content-Language"sv,
               "Content-Length"sv,
               "Content-Type"sv,
               "Expires"sv,
               "Last-Modified"sv,
               "Pragma"sv)
        || any_of(list, [&](auto list_header_name) {
               return StringView { header_name }.equals_ignoring_ascii_case(list_header_name)
                   && !is_forbidden_response_header_name(list_header_name);
           });
}

// https://fetch.spec.whatwg.org/#no-cors-safelisted-request-header-name
bool is_no_cors_safelisted_request_header_name(ReadonlyBytes header_name)
{
    // A no-CORS-safelisted request-header name is a header name that is a byte-case-insensitive match for one of
    // - `Accept`
    // - `Accept-Language`
    // - `Content-Language`
    // - `Content-Type`
    return StringView { header_name }.is_one_of_ignoring_ascii_case(
        "Accept"sv,
        "Accept-Language"sv,
        "Content-Language"sv,
        "Content-Type"sv);
}

// https://fetch.spec.whatwg.org/#no-cors-safelisted-request-header
bool is_no_cors_safelisted_request_header(Header const& header)
{
    // To determine whether a header (name, value) is a no-CORS-safelisted request-header, run these steps:

    // 1. If name is not a no-CORS-safelisted request-header name, then return false.
    if (!is_no_cors_safelisted_request_header_name(header.name))
        return false;

    // 2. Return whether (name, value) is a CORS-safelisted request-header.
    return is_cors_safelisted_request_header(header);
}

// https://fetch.spec.whatwg.org/#forbidden-header-name
bool is_forbidden_request_header(Header const& header)
{
    // A header (name, value) is forbidden request-header if these steps return true:
    auto name = StringView { header.name };

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
        auto parsed_values = get_decode_and_split_header_value(header.value);

        // 2. For each method of parsedValues: if the isomorphic encoding of method is a forbidden method, then return true.
        if (parsed_values.has_value() && any_of(*parsed_values, [](auto method) { return is_forbidden_method(method.bytes()); }))
            return true;
    }

    // 4. Return false.
    return false;
}

// https://fetch.spec.whatwg.org/#forbidden-response-header-name
bool is_forbidden_response_header_name(ReadonlyBytes header_name)
{
    // A forbidden response-header name is a header name that is a byte-case-insensitive match for one of:
    // - `Set-Cookie`
    // - `Set-Cookie2`
    return StringView { header_name }.is_one_of_ignoring_ascii_case(
        "Set-Cookie"sv,
        "Set-Cookie2"sv);
}

// https://fetch.spec.whatwg.org/#request-body-header-name
bool is_request_body_header_name(ReadonlyBytes header_name)
{
    // A request-body-header name is a header name that is a byte-case-insensitive match for one of:
    // - `Content-Encoding`
    // - `Content-Language`
    // - `Content-Location`
    // - `Content-Type`
    return StringView { header_name }.is_one_of_ignoring_ascii_case(
        "Content-Encoding"sv,
        "Content-Language"sv,
        "Content-Location"sv,
        "Content-Type"sv);
}

// https://fetch.spec.whatwg.org/#extract-header-values
Optional<Vector<ByteBuffer>> extract_header_values(Header const& header)
{
    // FIXME: 1. If parsing header’s value, per the ABNF for header’s name, fails, then return failure.
    // FIXME: 2. Return one or more values resulting from parsing header’s value, per the ABNF for header’s name.

    // For now we only parse some headers that are of the ABNF list form "#something"
    if (StringView { header.name }.is_one_of_ignoring_ascii_case(
            "Access-Control-Request-Headers"sv,
            "Access-Control-Expose-Headers"sv,
            "Access-Control-Allow-Headers"sv,
            "Access-Control-Allow-Methods"sv)
        && !header.value.is_empty()) {
        auto split_values = StringView { header.value }.split_view(',');
        Vector<ByteBuffer> trimmed_values;

        for (auto const& value : split_values) {
            auto trimmed_value = value.trim(" \t"sv);
            auto trimmed_value_as_byte_buffer = MUST(ByteBuffer::copy(trimmed_value.bytes()));
            trimmed_values.append(move(trimmed_value_as_byte_buffer));
        }

        return trimmed_values;
    }

    // This always ignores the ABNF rules for now and returns the header value as a single list item.
    return Vector { MUST(ByteBuffer::copy(header.value)) };
}

// https://fetch.spec.whatwg.org/#extract-header-list-values
Variant<Vector<ByteBuffer>, ExtractHeaderParseFailure, Empty> extract_header_list_values(ReadonlyBytes name, HeaderList const& list)
{
    // 1. If list does not contain name, then return null.
    if (!list.contains(name))
        return Empty {};

    // FIXME: 2. If the ABNF for name allows a single header and list contains more than one, then return failure.
    // NOTE: If different error handling is needed, extract the desired header first.

    // 3. Let values be an empty list.
    auto values = Vector<ByteBuffer> {};

    // 4. For each header header list contains whose name is name:
    for (auto const& header : list) {
        if (!StringView { header.name }.equals_ignoring_ascii_case(name))
            continue;

        // 1. Let extract be the result of extracting header values from header.
        auto extract = extract_header_values(header);

        // 2. If extract is failure, then return failure.
        if (!extract.has_value())
            return ExtractHeaderParseFailure {};

        // 3. Append each value in extract, in order, to values.
        values.extend(extract.release_value());
    }

    // 5. Return values.
    return values;
}

// https://fetch.spec.whatwg.org/#simple-range-header-value
Optional<RangeHeaderValue> parse_single_range_header_value(ReadonlyBytes value)
{
    // 1. Let data be the isomorphic decoding of value.
    auto data = StringView { value };

    // 2. If data does not start with "bytes=", then return failure.
    if (!data.starts_with("bytes="sv))
        return {};

    // 3. Let position be a position variable for data, initially pointing at the 6th code point of data.
    auto lexer = GenericLexer { data };
    lexer.ignore(6);

    // 4. Let rangeStart be the result of collecting a sequence of code points that are ASCII digits, from data given position.
    auto range_start = lexer.consume_while(is_ascii_digit);

    // 5. Let rangeStartValue be rangeStart, interpreted as decimal number, if rangeStart is not the empty string; otherwise null.
    auto range_start_value = range_start.to_number<u64>();

    // 6. If the code point at position within data is not U+002D (-), then return failure.
    // 7. Advance position by 1.
    if (!lexer.consume_specific('-'))
        return {};

    // 8. Let rangeEnd be the result of collecting a sequence of code points that are ASCII digits, from data given position.
    auto range_end = lexer.consume_while(is_ascii_digit);

    // 9. Let rangeEndValue be rangeEnd, interpreted as decimal number, if rangeEnd is not the empty string; otherwise null.
    auto range_end_value = range_end.to_number<u64>();

    // 10. If position is not past the end of data, then return failure.
    if (!lexer.is_eof())
        return {};

    // 11. If rangeEndValue and rangeStartValue are null, then return failure.
    if (!range_end_value.has_value() && !range_start_value.has_value())
        return {};

    // 12. If rangeStartValue and rangeEndValue are numbers, and rangeStartValue is greater than rangeEndValue, then return failure.
    if (range_start_value.has_value() && range_end_value.has_value() && *range_start_value > *range_end_value)
        return {};

    // 13. Return (rangeStartValue, rangeEndValue).
    // NOTE: The range end or start can be omitted, e.g., `bytes=0-` or `bytes=-500` are valid ranges.
    return RangeHeaderValue { move(range_start_value), move(range_end_value) };
}

// https://fetch.spec.whatwg.org/#default-user-agent-value
ByteBuffer default_user_agent_value()
{
    // A default `User-Agent` value is an implementation-defined header value for the `User-Agent` header.
    return MUST(ByteBuffer::copy(ResourceLoader::the().user_agent().bytes()));
}

}
