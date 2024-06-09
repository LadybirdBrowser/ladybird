/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HttpResponse.h>

namespace HTTP {

HttpResponse::HttpResponse(int code, HeaderMap&& headers, size_t size)
    : m_code(code)
    , m_headers(move(headers))
    , m_downloaded_size(size)
{
}

StringView HttpResponse::reason_phrase_for_code(int code)
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
