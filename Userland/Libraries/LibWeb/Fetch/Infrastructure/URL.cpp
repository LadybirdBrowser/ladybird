/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Karol Kosek <krkk@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <LibURL/URL.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#is-local
bool is_local_url(URL::URL const& url)
{
    // A URL is local if its scheme is a local scheme.
    return any_of(LOCAL_SCHEMES, [&](auto scheme) { return url.scheme() == scheme; });
}

// https://fetch.spec.whatwg.org/#fetch-scheme
bool is_fetch_scheme(StringView scheme)
{
    // A fetch scheme is "about", "blob", "data", "file", or an HTTP(S) scheme.
    return any_of(FETCH_SCHEMES, [&](auto fetch_scheme) { return scheme == fetch_scheme; });
}

// https://fetch.spec.whatwg.org/#http-scheme
bool is_http_or_https_scheme(StringView scheme)
{
    // An HTTP(S) scheme is "http" or "https".
    return any_of(HTTP_SCHEMES, [&](auto http_scheme) { return scheme == http_scheme; });
}

// https://fetch.spec.whatwg.org/#data-url-processor
ErrorOr<DataURL> process_data_url(URL::URL const& data_url)
{
    // 1. Assert: dataURL’s scheme is "data".
    VERIFY(data_url.scheme() == "data");

    // 2. Let input be the result of running the URL serializer on dataURL with exclude fragment set to true.
    auto input_serialized = data_url.serialize(URL::ExcludeFragment::Yes);
    StringView input = input_serialized;

    // 3. Remove the leading "data:" from input.
    input = input.substring_view("data:"sv.length());

    // 4. Let position point at the start of input.

    // 5. Let mimeType be the result of collecting a sequence of code points that are not equal to U+002C (,), given position.
    auto position = input.find(',');
    auto mime_type = input.substring_view(0, position.value_or(input.length()));

    // 6. Strip leading and trailing ASCII whitespace from mimeType.
    mime_type = mime_type.trim_whitespace(TrimMode::Both);

    // 7. If position is past the end of input, then return failure.
    if (!position.has_value())
        return Error::from_string_literal("Missing a comma character");

    // 8. Advance position by 1.
    position = position.value() + 1;

    // 9. Let encodedBody be the remainder of input.
    auto encoded_body = input.substring_view(position.value());

    // 10. Let body be the percent-decoding of encodedBody.
    auto body = URL::percent_decode(encoded_body).to_byte_buffer();

    // 11. If mimeType ends with U+003B (;), followed by zero or more U+0020 SPACE, followed by an ASCII case-insensitive match for "base64", then:
    if (mime_type.ends_with("base64"sv, CaseSensitivity::CaseInsensitive)) {
        auto trimmed_substring_view = mime_type.substring_view(0, mime_type.length() - 6);
        trimmed_substring_view = trimmed_substring_view.trim(" "sv, TrimMode::Right);
        if (trimmed_substring_view.ends_with(';')) {
            // 1. Let stringBody be the isomorphic decode of body.
            auto string_body = StringView(body);

            // 2. Set body to the forgiving-base64 decode of stringBody.
            // 3. If body is failure, then return failure.
            body = TRY(decode_base64(string_body));

            // 4. Remove the last 6 code points from mimeType.
            // 5. Remove trailing U+0020 SPACE code points from mimeType, if any.
            // 6. Remove the last U+003B (;) from mimeType.
            mime_type = trimmed_substring_view.substring_view(0, trimmed_substring_view.length() - 1);
        }
    }

    // 12. If mimeType starts with ";", then prepend "text/plain" to mimeType.
    StringBuilder builder;
    if (mime_type.starts_with(';')) {
        builder.append("text/plain"sv);
        builder.append(mime_type);
        mime_type = builder.string_view();
    }

    // 13. Let mimeTypeRecord be the result of parsing mimeType.
    auto mime_type_record = TRY(MimeSniff::MimeType::parse(mime_type));

    // 14. If mimeTypeRecord is failure, then set mimeTypeRecord to text/plain;charset=US-ASCII.
    if (!mime_type_record.has_value()) {
        mime_type_record = TRY(MimeSniff::MimeType::create("text"_string, "plain"_string));
        TRY(mime_type_record->set_parameter("charset"_string, "US-ASCII"_string));
    }

    // 15. Return a new data: URL struct whose MIME type is mimeTypeRecord and body is body.
    return DataURL { mime_type_record.release_value(), body };
}

}
