/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HeaderList.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#concept-header-extract-mime-type
Optional<MimeSniff::MimeType> extract_mime_type(HTTP::HeaderList const& headers)
{
    // 1. Let charset be null.
    Optional<String> charset;

    // 2. Let essence be null.
    Optional<String> essence;

    // 3. Let mimeType be null.
    Optional<MimeSniff::MimeType> mime_type;

    // 4. Let values be the result of getting, decoding, and splitting `Content-Type` from headers.
    auto values = headers.get_decode_and_split("Content-Type"sv);

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
        if (!essence.has_value() || (mime_type->essence() != *essence)) {
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

}
