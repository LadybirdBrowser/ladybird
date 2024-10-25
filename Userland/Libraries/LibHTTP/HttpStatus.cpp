/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/HashMap.h>
#include <LibHTTP/HttpStatus.h>

namespace HTTP {

HttpStatus const HttpStatus::OK = HttpStatus::for_code(200);
HttpStatus const HttpStatus::BAD_REQUEST = HttpStatus::for_code(400);
HttpStatus const HttpStatus::INTERNAL_SERVER_ERROR = HttpStatus::for_code(500);

HttpStatus HttpStatus::for_code(u16 code)
{
    return HttpStatus {
        .code = code,
        .reason_phrase = MUST(ByteBuffer::copy(HttpStatus::reason_phrase_for_code(code).bytes())),
    };
}
StringView HttpStatus::reason_phrase_for_code(u16 code)
{
    VERIFY(code >= 100 && code <= 599);

    static HashMap<int, StringView> s_reason_phrases = {
        { 100, "Continue"sv },
        { 101, "Switching Protocols"sv },
        { 200, "OK"sv },
        { 201, "Created"sv },
        { 202, "Accepted"sv },
        { 203, "Non-Authoritative Information"sv },
        { 204, "No Content"sv },
        { 205, "Reset Content"sv },
        { 206, "Partial Content"sv },
        { 300, "Multiple Choices"sv },
        { 301, "Moved Permanently"sv },
        { 302, "Found"sv },
        { 303, "See Other"sv },
        { 304, "Not Modified"sv },
        { 305, "Use Proxy"sv },
        { 307, "Temporary Redirect"sv },
        { 400, "Bad Request"sv },
        { 401, "Unauthorized"sv },
        { 402, "Payment Required"sv },
        { 403, "Forbidden"sv },
        { 404, "Not Found"sv },
        { 405, "Method Not Allowed"sv },
        { 406, "Not Acceptable"sv },
        { 407, "Proxy Authentication Required"sv },
        { 408, "Request Timeout"sv },
        { 409, "Conflict"sv },
        { 410, "Gone"sv },
        { 411, "Length Required"sv },
        { 412, "Precondition Failed"sv },
        { 413, "Payload Too Large"sv },
        { 414, "URI Too Long"sv },
        { 415, "Unsupported Media Type"sv },
        { 416, "Range Not Satisfiable"sv },
        { 417, "Expectation Failed"sv },
        { 426, "Upgrade Required"sv },
        { 500, "Internal Server Error"sv },
        { 501, "Not Implemented"sv },
        { 502, "Bad Gateway"sv },
        { 503, "Service Unavailable"sv },
        { 504, "Gateway Timeout"sv },
        { 505, "HTTP Version Not Supported"sv }
    };

    if (s_reason_phrases.contains(code))
        return s_reason_phrases.ensure(code);

    // NOTE: "A client MUST understand the class of any status code, as indicated by the first
    //       digit, and treat an unrecognized status code as being equivalent to the x00 status
    //       code of that class." (RFC 7231, section 6)
    auto generic_code = (code / 100) * 100;
    VERIFY(s_reason_phrases.contains(generic_code));
    return s_reason_phrases.ensure(generic_code);
}

}

namespace AK {

ErrorOr<void> Formatter<HTTP::HttpStatus>::format(FormatBuilder& builder, HTTP::HttpStatus const& status)
{
    TRY(builder.put_u64(status.code));
    if (!status.reason_phrase.is_empty()) {
        TRY(builder.put_literal(" "sv));
        TRY(builder.put_string(status.reason_phrase.span()));
    }
    return {};
}

}
