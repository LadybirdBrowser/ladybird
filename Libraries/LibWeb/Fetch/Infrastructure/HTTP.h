/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/StringView.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#http-tab-or-space
// An HTTP tab or space is U+0009 TAB or U+0020 SPACE.
inline constexpr StringView HTTP_TAB_OR_SPACE = "\t "sv;

// https://fetch.spec.whatwg.org/#http-whitespace
// HTTP whitespace is U+000A LF, U+000D CR, or an HTTP tab or space.
inline constexpr StringView HTTP_WHITESPACE = "\n\r\t "sv;

// https://fetch.spec.whatwg.org/#http-newline-byte
// An HTTP newline byte is 0x0A (LF) or 0x0D (CR).
inline constexpr Array HTTP_NEWLINE_BYTES = {
    0x0A, 0x0D
};

// https://fetch.spec.whatwg.org/#http-tab-or-space-byte
// An HTTP tab or space byte is 0x09 (HT) or 0x20 (SP).
inline constexpr Array HTTP_TAB_OR_SPACE_BYTES = {
    0x09, 0x20
};

enum class HttpQuotedStringExtractValue {
    No,
    Yes,
};

[[nodiscard]] String collect_an_http_quoted_string(GenericLexer& lexer, HttpQuotedStringExtractValue extract_value = HttpQuotedStringExtractValue::No);

}
