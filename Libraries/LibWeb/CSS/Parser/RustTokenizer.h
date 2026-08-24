/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <LibWeb/Export.h>

namespace Web::CSS::Parser {

enum class TokenizerInput {
    DecodedText,
    EncodedBytes,
};

class WEB_API RustTokenizer {
public:
    static bool input_needs_normalization(Utf16View);
    static Utf16String normalize_input(StringView input, StringView encoding, TokenizerInput = TokenizerInput::DecodedText);
    static Utf16String normalize_input(Utf16View);
};

}
