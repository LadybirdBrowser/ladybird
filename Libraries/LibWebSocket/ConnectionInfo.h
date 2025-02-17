/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibHTTP/HeaderMap.h>
#include <LibURL/URL.h>

namespace WebSocket {

class ConnectionInfo final {
public:
    ConnectionInfo(URL::URL);

    URL::URL const& url() const { return m_url; }

    ByteString const& origin() const { return m_origin; }
    void set_origin(ByteString origin) { m_origin = move(origin); }

    Vector<ByteString> const& protocols() const { return m_protocols; }
    void set_protocols(Vector<ByteString> protocols) { m_protocols = move(protocols); }

    Vector<ByteString> const& extensions() const { return m_extensions; }
    void set_extensions(Vector<ByteString> extensions) { m_extensions = move(extensions); }

    HTTP::HeaderMap const& headers() const { return m_headers; }
    void set_headers(HTTP::HeaderMap headers) { m_headers = move(headers); }

    Optional<ByteString> const& root_certificates_path() const { return m_root_certificates_path; }
    void set_root_certificates_path(Optional<ByteString> root_certificates_path) { m_root_certificates_path = move(root_certificates_path); }

    // secure flag - defined in RFC 6455 Section 3
    bool is_secure() const;

    // "resource-name" or "/resource name/" - defined in RFC 6455 Section 3
    ByteString resource_name() const;

private:
    URL::URL m_url;
    ByteString m_origin;
    Vector<ByteString> m_protocols {};
    Vector<ByteString> m_extensions {};
    HTTP::HeaderMap m_headers;
    Optional<ByteString> m_root_certificates_path;
};

}
