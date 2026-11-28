/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTextCodec/Decoder.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>

namespace Web::Fetch::Infrastructure {

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
