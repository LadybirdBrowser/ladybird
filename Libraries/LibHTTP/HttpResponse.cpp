/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
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
        { 100, "Continue"_sv },
        { 101, "Switching Protocols"_sv },
        { 200, "OK"_sv },
        { 201, "Created"_sv },
        { 202, "Accepted"_sv },
        { 203, "Non-Authoritative Information"_sv },
        { 204, "No Content"_sv },
        { 205, "Reset Content"_sv },
        { 206, "Partial Content"_sv },
        { 300, "Multiple Choices"_sv },
        { 301, "Moved Permanently"_sv },
        { 302, "Found"_sv },
        { 303, "See Other"_sv },
        { 304, "Not Modified"_sv },
        { 305, "Use Proxy"_sv },
        { 307, "Temporary Redirect"_sv },
        { 400, "Bad Request"_sv },
        { 401, "Unauthorized"_sv },
        { 402, "Payment Required"_sv },
        { 403, "Forbidden"_sv },
        { 404, "Not Found"_sv },
        { 405, "Method Not Allowed"_sv },
        { 406, "Not Acceptable"_sv },
        { 407, "Proxy Authentication Required"_sv },
        { 408, "Request Timeout"_sv },
        { 409, "Conflict"_sv },
        { 410, "Gone"_sv },
        { 411, "Length Required"_sv },
        { 412, "Precondition Failed"_sv },
        { 413, "Payload Too Large"_sv },
        { 414, "URI Too Long"_sv },
        { 415, "Unsupported Media Type"_sv },
        { 416, "Range Not Satisfiable"_sv },
        { 417, "Expectation Failed"_sv },
        { 418, "I'm a teapot"_sv },
        { 421, "Misdirected Request"_sv },
        { 422, "Unprocessable Content"_sv },
        { 423, "Locked"_sv },
        { 424, "Failed Dependency"_sv },
        { 425, "Too Early"_sv },
        { 426, "Upgrade Required"_sv },
        { 428, "Precondition Required"_sv },
        { 429, "Too Many Requests"_sv },
        { 431, "Request Header Fields Too Large"_sv },
        { 451, "Unavailable For Legal Reasons"_sv },
        { 500, "Internal Server Error"_sv },
        { 501, "Not Implemented"_sv },
        { 502, "Bad Gateway"_sv },
        { 503, "Service Unavailable"_sv },
        { 504, "Gateway Timeout"_sv },
        { 505, "HTTP Version Not Supported"_sv }
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
