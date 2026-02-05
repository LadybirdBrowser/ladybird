/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace HTTP {

constexpr StringView reason_phrase_for_code(u32 code)
{
    VERIFY(code >= 100 && code <= 599);

    // clang-format off
    switch (code) {
    case 100: return "Continue"sv;
    case 101: return "Switching Protocols"sv;
    case 200: return "OK"sv;
    case 201: return "Created"sv;
    case 202: return "Accepted"sv;
    case 203: return "Non-Authoritative Information"sv;
    case 204: return "No Content"sv;
    case 205: return "Reset Content"sv;
    case 206: return "Partial Content"sv;
    case 300: return "Multiple Choices"sv;
    case 301: return "Moved Permanently"sv;
    case 302: return "Found"sv;
    case 303: return "See Other"sv;
    case 304: return "Not Modified"sv;
    case 305: return "Use Proxy"sv;
    case 307: return "Temporary Redirect"sv;
    case 400: return "Bad Request"sv;
    case 401: return "Unauthorized"sv;
    case 402: return "Payment Required"sv;
    case 403: return "Forbidden"sv;
    case 404: return "Not Found"sv;
    case 405: return "Method Not Allowed"sv;
    case 406: return "Not Acceptable"sv;
    case 407: return "Proxy Authentication Required"sv;
    case 408: return "Request Timeout"sv;
    case 409: return "Conflict"sv;
    case 410: return "Gone"sv;
    case 411: return "Length Required"sv;
    case 412: return "Precondition Failed"sv;
    case 413: return "Payload Too Large"sv;
    case 414: return "URI Too Long"sv;
    case 415: return "Unsupported Media Type"sv;
    case 416: return "Range Not Satisfiable"sv;
    case 417: return "Expectation Failed"sv;
    case 418: return "I'm a teapot"sv;
    case 421: return "Misdirected Request"sv;
    case 422: return "Unprocessable Content"sv;
    case 423: return "Locked"sv;
    case 424: return "Failed Dependency"sv;
    case 425: return "Too Early"sv;
    case 426: return "Upgrade Required"sv;
    case 428: return "Precondition Required"sv;
    case 429: return "Too Many Requests"sv;
    case 431: return "Request Header Fields Too Large"sv;
    case 451: return "Unavailable For Legal Reasons"sv;
    case 500: return "Internal Server Error"sv;
    case 501: return "Not Implemented"sv;
    case 502: return "Bad Gateway"sv;
    case 503: return "Service Unavailable"sv;
    case 504: return "Gateway Timeout"sv;
    case 505: return "HTTP Version Not Supported"sv;
    default: break;
    }
    // clang-format on

    // A client MUST understand the class of any status code, as indicated by the first digit, and treat an unrecognized
    // status code as being equivalent to the x00 status code of that class. (RFC 7231, section 6)
    return reason_phrase_for_code((code / 100) * 100);
}

}
