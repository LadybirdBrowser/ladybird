/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGC/Heap.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Export.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#concept-header
// A header is a tuple that consists of a name (a header name) and value (a header value).
struct WEB_API Header {
    [[nodiscard]] static Header isomorphic_encode(StringView, StringView);

    Optional<Vector<ByteString>> extract_header_values() const;

    ByteString name;
    ByteString value;
};

// https://fetch.spec.whatwg.org/#concept-header-list
// A header list is a list of zero or more headers. It is initially the empty list.
class WEB_API HeaderList final
    : public JS::Cell
    , public Vector<Header> {
    GC_CELL(HeaderList, JS::Cell);
    GC_DECLARE_ALLOCATOR(HeaderList);

public:
    [[nodiscard]] static GC::Ref<HeaderList> create(JS::VM&);

    using Vector::begin;
    using Vector::clear;
    using Vector::end;
    using Vector::is_empty;

    [[nodiscard]] bool contains(StringView) const;
    [[nodiscard]] Optional<ByteString> get(StringView) const;
    [[nodiscard]] Optional<Vector<String>> get_decode_and_split(StringView) const;
    void append(Header);
    void delete_(StringView name);
    void set(Header);
    void combine(Header);
    [[nodiscard]] Vector<Header> sort_and_combine() const;

    struct ExtractHeaderParseFailure { };
    [[nodiscard]] Variant<Empty, Vector<ByteString>, ExtractHeaderParseFailure> extract_header_list_values(StringView) const;

    struct ExtractLengthFailure { };
    [[nodiscard]] Variant<Empty, u64, ExtractLengthFailure> extract_length() const;

    [[nodiscard]] Optional<MimeSniff::MimeType> extract_mime_type() const;

    [[nodiscard]] Vector<ByteString> unique_names() const;
};

struct RangeHeaderValue {
    Optional<u64> start;
    Optional<u64> end;
};

[[nodiscard]] bool is_header_name(StringView);
[[nodiscard]] bool is_header_value(StringView);
[[nodiscard]] ByteString normalize_header_value(StringView);

[[nodiscard]] bool is_forbidden_request_header(Header const&);
[[nodiscard]] bool is_forbidden_response_header_name(StringView);

[[nodiscard]] WEB_API StringView legacy_extract_an_encoding(Optional<MimeSniff::MimeType> const& mime_type, StringView fallback_encoding);
[[nodiscard]] Vector<String> get_decode_and_split_header_value(StringView);
[[nodiscard]] Vector<ByteString> convert_header_names_to_a_sorted_lowercase_set(ReadonlySpan<ByteString>);

[[nodiscard]] WEB_API ByteString build_content_range(u64 range_start, u64 range_end, u64 full_length);
[[nodiscard]] WEB_API Optional<RangeHeaderValue> parse_single_range_header_value(StringView, bool);

[[nodiscard]] bool is_cors_safelisted_request_header(Header const&);
[[nodiscard]] bool is_cors_unsafe_request_header_byte(u8);
[[nodiscard]] WEB_API Vector<ByteString> get_cors_unsafe_header_names(HeaderList const&);
[[nodiscard]] WEB_API bool is_cors_non_wildcard_request_header_name(StringView);
[[nodiscard]] bool is_privileged_no_cors_request_header_name(StringView);
[[nodiscard]] bool is_cors_safelisted_response_header_name(StringView, ReadonlySpan<StringView>);
[[nodiscard]] bool is_no_cors_safelisted_request_header_name(StringView);
[[nodiscard]] bool is_no_cors_safelisted_request_header(Header const&);

[[nodiscard]] WEB_API ByteString const& default_user_agent_value();

}
