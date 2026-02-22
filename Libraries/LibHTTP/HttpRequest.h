/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Noncopyable.h>
#include <LibHTTP/HeaderList.h>
#include <LibURL/URL.h>

namespace HTTP {

class HttpRequest {
public:
    enum class ParseError {
        RequestTooLarge,
        RequestIncomplete,
        OutOfMemory,
        UnsupportedMethod,
        InvalidURL
    };

    static StringView parse_error_to_string(ParseError error)
    {
        switch (error) {
        case ParseError::RequestTooLarge:
            return "Request too large"sv;
        case ParseError::RequestIncomplete:
            return "Request is incomplete"sv;
        case ParseError::OutOfMemory:
            return "Out of memory"sv;
        case ParseError::UnsupportedMethod:
            return "Unsupported method"sv;
        case ParseError::InvalidURL:
            return "Invalid URL"sv;
        }
        VERIFY_NOT_REACHED();
    }

    enum Method {
        Invalid,
        HEAD,
        GET,
        POST,
        DELETE,
        PATCH,
        OPTIONS,
        TRACE,
        CONNECT,
        PUT,
    };

    explicit HttpRequest(NonnullRefPtr<HeaderList>);
    ~HttpRequest() = default;

    AK_MAKE_DEFAULT_MOVABLE(HttpRequest);

    ByteString const& resource() const { return m_resource; }
    HeaderList const& headers() const { return m_headers; }

    URL::URL const& url() const { return m_url; }
    void set_url(URL::URL const& url) { m_url = url; }

    Method method() const { return m_method; }
    void set_method(Method method) { m_method = method; }

    ByteBuffer const& body() const { return m_body; }
    void set_body(ByteBuffer&& body) { m_body = move(body); }

    StringView method_name() const;
    ErrorOr<ByteBuffer> to_raw_request() const;

    static ErrorOr<HttpRequest, HttpRequest::ParseError> from_raw_request(ReadonlyBytes);

private:
    URL::URL m_url;
    ByteString m_resource;
    Method m_method { GET };
    NonnullRefPtr<HeaderList> m_headers;
    ByteBuffer m_body;
};

StringView to_string_view(HttpRequest::Method);

}
