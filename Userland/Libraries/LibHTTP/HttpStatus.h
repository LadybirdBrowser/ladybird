/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Format.h>

namespace HTTP {

struct HttpStatus {
    u16 code { 0 };
    ByteBuffer reason_phrase;

    static HttpStatus const OK;
    static HttpStatus const BAD_REQUEST;
    static HttpStatus const INTERNAL_SERVER_ERROR;

    static HttpStatus for_code(u16 code);
    static StringView reason_phrase_for_code(u16 code);
};

}

namespace AK {

template<>
struct Formatter<HTTP::HttpStatus> : StandardFormatter {
    ErrorOr<void> format(FormatBuilder&, HTTP::HttpStatus const&);
};

}
