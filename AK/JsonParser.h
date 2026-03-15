/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericLexer.h>
#include <AK/JsonValue.h>

namespace AK {

class JsonParser : private GenericLexer {
public:
    static ErrorOr<JsonValue> parse(StringView);

private:
    explicit JsonParser(StringView input)
        : GenericLexer(input)
    {
    }

    ErrorOr<JsonValue> parse_json();
    ErrorOr<JsonValue> parse_helper();

    ErrorOr<ByteString> consume_and_unescape_string();
    ErrorOr<JsonValue> parse_array();
    ErrorOr<JsonValue> parse_object();
    ErrorOr<JsonValue> parse_number();
    ErrorOr<JsonValue> parse_string();
    ErrorOr<JsonValue> parse_false();
    ErrorOr<JsonValue> parse_true();
    ErrorOr<JsonValue> parse_null();

    // Keep recursive parsing depth bounded so untrusted JSON cannot overflow the call stack.
    static constexpr size_t max_nesting_depth { 512 };
    size_t m_current_nesting_depth { 0 };
};

}

#if USING_AK_GLOBALLY
using AK::JsonParser;
#endif
