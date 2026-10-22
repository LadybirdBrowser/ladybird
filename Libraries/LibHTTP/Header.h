/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibIPC/Forward.h>
#include <LibURL/URL.h>

namespace HTTP {

// https://fetch.spec.whatwg.org/#concept-header
struct Header {
    [[nodiscard]] static Header isomorphic_encode(StringView, StringView);
    [[nodiscard]] static Header isomorphic_encode(StringView, Utf16View);

    Optional<Vector<ByteString>> extract_header_values() const;

    ByteString name;
    ByteString value;
};

[[nodiscard]] bool is_header_name(StringView);
[[nodiscard]] bool is_header_value(StringView);
[[nodiscard]] StringView normalize_header_value(StringView);

[[nodiscard]] bool is_forbidden_request_header(Header const&);
[[nodiscard]] bool is_forbidden_response_header_name(StringView);

[[nodiscard]] Vector<String> get_decode_and_split_header_value(StringView value);
[[nodiscard]] Vector<ByteString> convert_header_names_to_a_sorted_lowercase_set(ReadonlySpan<ByteString> header_names);

struct RangeHeaderValue {
    Optional<u64> start;
    Optional<u64> end;
};

[[nodiscard]] ByteString build_content_range(u64 range_start, u64 range_end, u64 full_length);
Optional<RangeHeaderValue> parse_single_range_header_value(StringView, bool allow_whitespace);

struct LinkParameter {
    String name;
    String value;
};

struct LinkTargetAttributes : public RefCounted<LinkTargetAttributes> {
    explicit LinkTargetAttributes(Vector<LinkParameter> attributes)
        : attributes(move(attributes))
    {
    }

    Vector<LinkParameter> attributes;
};

struct Link {
    Optional<String const&> target_attribute(StringView name) const;

    Optional<URL::URL> target_uri;
    String relation_type;
    Optional<URL::URL> context_uri;
    NonnullRefPtr<LinkTargetAttributes const> target_attributes;
};

[[nodiscard]] Vector<String> get_decode_and_split_link_header_value(StringView);
[[nodiscard]] Vector<Link> parse_link_field_value(StringView, URL::URL const& representation_url);

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, HTTP::Header const& header);

template<>
ErrorOr<HTTP::Header> decode(Decoder& decoder);

}
