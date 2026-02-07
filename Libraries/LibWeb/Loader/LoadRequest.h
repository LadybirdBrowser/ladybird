/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Time.h>
#include <LibCore/ElapsedTimer.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Cookie/IncludeCredentials.h>
#include <LibHTTP/HeaderList.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Page/Page.h>

namespace Web {

class WEB_API LoadRequest {
public:
    explicit LoadRequest(NonnullRefPtr<HTTP::HeaderList> headers)
        : m_headers(move(headers))
    {
    }

    Optional<URL::URL> const& url() const { return m_url; }
    void set_url(Optional<URL::URL> url) { m_url = move(url); }

    ByteString const& method() const { return m_method; }
    void set_method(ByteString const& method) { m_method = method; }

    ByteBuffer const& body() const { return m_body; }
    void set_body(ByteBuffer body) { m_body = move(body); }

    HTTP::CacheMode cache_mode() const { return m_cache_mode; }
    void set_cache_mode(HTTP::CacheMode cache_mode) { m_cache_mode = cache_mode; }

    HTTP::Cookie::IncludeCredentials include_credentials() const { return m_include_credentials; }
    void set_include_credentials(HTTP::Cookie::IncludeCredentials include_credentials) { m_include_credentials = include_credentials; }

    Optional<Fetch::Infrastructure::Request::InitiatorType> const& initiator_type() const { return m_initiator_type; }
    void set_initiator_type(Optional<Fetch::Infrastructure::Request::InitiatorType> initiator_type) { m_initiator_type = move(initiator_type); }

    void start_timer() { m_load_timer.start(); }
    AK::Duration load_time() const { return m_load_timer.elapsed_time(); }

    GC::Ptr<Page> page() const { return m_page.ptr(); }
    void set_page(Page& page) { m_page = page; }

    HTTP::HeaderList const& headers() const { return m_headers; }

private:
    Optional<URL::URL> m_url;
    ByteString m_method { "GET" };
    NonnullRefPtr<HTTP::HeaderList> m_headers;
    ByteBuffer m_body;
    Core::ElapsedTimer m_load_timer;
    GC::Root<Page> m_page;
    HTTP::CacheMode m_cache_mode { HTTP::CacheMode::Default };
    HTTP::Cookie::IncludeCredentials m_include_credentials { HTTP::Cookie::IncludeCredentials::Yes };
    Optional<Fetch::Infrastructure::Request::InitiatorType> m_initiator_type;
};

}
