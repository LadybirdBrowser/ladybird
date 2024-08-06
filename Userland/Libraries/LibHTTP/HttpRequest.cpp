/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/StringBuilder.h>
#include <LibHTTP/HttpRequest.h>
#include <LibHTTP/Job.h>
#include <LibURL/Parser.h>

namespace HTTP {

StringView to_string_view(HttpRequest::Method method)
{
    switch (method) {
    case HttpRequest::Method::GET:
        return "GET"sv;
    case HttpRequest::Method::HEAD:
        return "HEAD"sv;
    case HttpRequest::Method::POST:
        return "POST"sv;
    case HttpRequest::Method::DELETE:
        return "DELETE"sv;
    case HttpRequest::Method::PATCH:
        return "PATCH"sv;
    case HttpRequest::Method::OPTIONS:
        return "OPTIONS"sv;
    case HttpRequest::Method::TRACE:
        return "TRACE"sv;
    case HttpRequest::Method::CONNECT:
        return "CONNECT"sv;
    case HttpRequest::Method::PUT:
        return "PUT"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

StringView HttpRequest::method_name() const
{
    return to_string_view(m_method);
}

ErrorOr<ByteBuffer> HttpRequest::to_raw_request() const
{
    StringBuilder builder;
    TRY(builder.try_append(method_name()));
    TRY(builder.try_append(' '));
    auto path = m_url.serialize_path();
    VERIFY(!path.is_empty());
    TRY(builder.try_append(path));
    if (m_url.query().has_value()) {
        TRY(builder.try_append('?'));
        TRY(builder.try_append(*m_url.query()));
    }
    TRY(builder.try_append(" HTTP/1.1\r\nHost: "sv));
    TRY(builder.try_append(TRY(m_url.serialized_host())));
    if (m_url.port().has_value())
        TRY(builder.try_appendff(":{}", *m_url.port()));
    TRY(builder.try_append("\r\n"sv));
    // Start headers.
    bool has_content_length = m_headers.contains("Content-Length"sv);
    for (auto const& [name, value] : m_headers.headers()) {
        TRY(builder.try_append(name));
        TRY(builder.try_append(": "sv));
        TRY(builder.try_append(value));
        TRY(builder.try_append("\r\n"sv));
    }
    if (!m_body.is_empty() || method() == Method::POST) {
        // Add Content-Length header if it's not already present.
        if (!has_content_length) {
            TRY(builder.try_appendff("Content-Length: {}\r\n", m_body.size()));
        }
        // Finish headers.
        TRY(builder.try_append("\r\n"sv));
        TRY(builder.try_append((char const*)m_body.data(), m_body.size()));
    } else {
        // Finish headers.
        TRY(builder.try_append("\r\n"sv));
    }
    return builder.to_byte_buffer();
}

ErrorOr<HttpRequest, HttpRequest::ParseError> HttpRequest::from_raw_request(ReadonlyBytes raw_request)
{
    enum class State {
        InMethod,
        InResource,
        InProtocol,
        InHeaderName,
        InHeaderValue,
        InBody,
    };

    State state { State::InMethod };
    size_t index = 0;

    auto peek = [&](int offset = 0) -> u8 {
        if (index + offset >= raw_request.size())
            return 0;
        return raw_request[index + offset];
    };

    auto consume = [&]() -> u8 {
        VERIFY(index < raw_request.size());
        return raw_request[index++];
    };

    Vector<u8, 256> buffer;

    Optional<unsigned> content_length;
    ByteString method;
    ByteString resource;
    ByteString protocol;
    HeaderMap headers;
    Header current_header;
    ByteBuffer body;

    auto commit_and_advance_to = [&](auto& output, State new_state) {
        output = ByteString::copy(buffer);
        buffer.clear();
        state = new_state;
    };

    while (index < raw_request.size()) {
        // FIXME: Figure out what the appropriate limitations should be.
        if (buffer.size() > 65536)
            return ParseError::RequestTooLarge;
        switch (state) {
        case State::InMethod:
            if (peek() == ' ') {
                consume();
                commit_and_advance_to(method, State::InResource);
                break;
            }
            buffer.append(consume());
            break;
        case State::InResource:
            if (peek() == ' ') {
                consume();
                commit_and_advance_to(resource, State::InProtocol);
                break;
            }
            buffer.append(consume());
            break;
        case State::InProtocol:
            if (peek(0) == '\r' && peek(1) == '\n') {
                consume();
                consume();
                commit_and_advance_to(protocol, State::InHeaderName);
                break;
            }
            buffer.append(consume());
            break;
        case State::InHeaderName:
            if (peek(0) == ':' && peek(1) == ' ') {
                consume();
                consume();
                commit_and_advance_to(current_header.name, State::InHeaderValue);
                break;
            }
            buffer.append(consume());
            break;
        case State::InHeaderValue:
            if (peek(0) == '\r' && peek(1) == '\n') {
                consume();
                consume();

                // Detect end of headers
                auto next_state = State::InHeaderName;
                if (peek(0) == '\r' && peek(1) == '\n') {
                    consume();
                    consume();
                    next_state = State::InBody;
                }

                commit_and_advance_to(current_header.value, next_state);

                if (current_header.name.equals_ignoring_ascii_case("Content-Length"sv))
                    content_length = current_header.value.to_number<unsigned>();

                headers.set(move(current_header.name), move(current_header.value));
                break;
            }
            buffer.append(consume());
            break;
        case State::InBody:
            buffer.append(consume());
            if (index == raw_request.size()) {
                // End of data, so store the body
                auto maybe_body = ByteBuffer::copy(buffer);
                if (maybe_body.is_error()) {
                    VERIFY(maybe_body.error().code() == ENOMEM);
                    return ParseError::OutOfMemory;
                }
                body = maybe_body.release_value();
                buffer.clear();
            }
            break;
        }
    }

    if (state != State::InBody)
        return ParseError::RequestIncomplete;

    if (content_length.has_value() && content_length.value() != body.size())
        return ParseError::RequestIncomplete;

    HttpRequest request;
    if (method == "GET")
        request.m_method = Method::GET;
    else if (method == "HEAD")
        request.m_method = Method::HEAD;
    else if (method == "POST")
        request.m_method = Method::POST;
    else if (method == "DELETE")
        request.set_method(HTTP::HttpRequest::Method::DELETE);
    else if (method == "PATCH")
        request.set_method(HTTP::HttpRequest::Method::PATCH);
    else if (method == "OPTIONS")
        request.set_method(HTTP::HttpRequest::Method::OPTIONS);
    else if (method == "TRACE")
        request.set_method(HTTP::HttpRequest::Method::TRACE);
    else if (method == "CONNECT")
        request.set_method(HTTP::HttpRequest::Method::CONNECT);
    else if (method == "PUT")
        request.set_method(HTTP::HttpRequest::Method::PUT);
    else
        return ParseError::UnsupportedMethod;

    request.m_headers = move(headers);
    auto url_parts = resource.split_limit('?', 2, SplitBehavior::KeepEmpty);

    auto url_part_to_string = [](ByteString const& url_part) -> ErrorOr<String, ParseError> {
        auto query_string_or_error = String::from_byte_string(url_part);
        if (!query_string_or_error.is_error())
            return query_string_or_error.release_value();

        if (query_string_or_error.error().code() == ENOMEM)
            return ParseError::OutOfMemory;

        return ParseError::InvalidURL;
    };

    request.m_url.set_cannot_be_a_base_url(true);
    if (url_parts.size() == 2) {
        request.m_resource = url_parts[0];
        request.m_url.set_paths({ url_parts[0] });
        request.m_url.set_query(TRY(url_part_to_string(url_parts[1])));
    } else {
        request.m_resource = resource;
        request.m_url.set_paths({ resource });
    }

    request.set_body(move(body));

    return request;
}

void HttpRequest::set_headers(HTTP::HeaderMap headers)
{
    m_headers = move(headers);
}

Optional<Header> HttpRequest::get_http_basic_authentication_header(URL::URL const& url)
{
    if (!url.includes_credentials())
        return {};
    StringBuilder builder;
    builder.append(URL::percent_decode(url.username()));
    builder.append(':');
    builder.append(URL::percent_decode(url.password()));

    // FIXME: change to TRY() and make method fallible
    auto token = MUST(encode_base64(builder.string_view().bytes()));
    builder.clear();
    builder.append("Basic "sv);
    builder.append(token);
    return Header { "Authorization", builder.to_byte_string() };
}

Optional<HttpRequest::BasicAuthenticationCredentials> HttpRequest::parse_http_basic_authentication_header(ByteString const& value)
{
    if (!value.starts_with("Basic "sv, AK::CaseSensitivity::CaseInsensitive))
        return {};
    auto token = value.substring_view(6);
    if (token.is_empty())
        return {};
    auto decoded_token_bb = decode_base64(token);
    if (decoded_token_bb.is_error())
        return {};
    auto decoded_token = ByteString::copy(decoded_token_bb.value());
    auto colon_index = decoded_token.find(':');
    if (!colon_index.has_value())
        return {};
    auto username = decoded_token.substring_view(0, colon_index.value());
    auto password = decoded_token.substring_view(colon_index.value() + 1);
    return BasicAuthenticationCredentials { username, password };
}

}
