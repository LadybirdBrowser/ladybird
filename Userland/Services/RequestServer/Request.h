/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <LibURL/URL.h>
#include <RequestServer/Forward.h>

namespace RequestServer {

class Request {
public:
    virtual ~Request() = default;

    i32 id() const { return m_id; }
    virtual URL::URL url() const = 0;

    Optional<u32> status_code() const { return m_status_code; }
    Optional<u64> total_size() const { return m_total_size; }
    size_t downloaded_size() const { return m_downloaded_size; }
    HTTP::HeaderMap const& response_headers() const { return m_response_headers; }

    void stop();
    virtual void set_certificate(ByteString, ByteString);

    // FIXME: Want Badge<Protocol>, but can't make one from HttpProtocol, etc.
    void set_request_fd(int fd) { m_request_fd = fd; }
    int request_fd() const { return m_request_fd; }

    void did_finish(bool success);
    void did_progress(Optional<u64> total_size, u64 downloaded_size);
    void set_status_code(u32 status_code) { m_status_code = status_code; }
    void did_request_certificates();
    void set_response_headers(HTTP::HeaderMap);
    void set_downloaded_size(size_t size) { m_downloaded_size = size; }
    Core::File const& output_stream() const { return *m_output_stream; }

protected:
    explicit Request(ConnectionFromClient&, NonnullOwnPtr<Core::File>&&, i32 request_id);

private:
    ConnectionFromClient& m_client;
    i32 m_id { 0 };
    int m_request_fd { -1 }; // Passed to client.
    Optional<u32> m_status_code;
    Optional<u64> m_total_size {};
    size_t m_downloaded_size { 0 };
    NonnullOwnPtr<Core::File> m_output_stream;
    HTTP::HeaderMap m_response_headers;
};

}
