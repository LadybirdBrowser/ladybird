/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Time.h>
#include <LibCore/ElapsedTimer.h>
#include <LibHTTP/HeaderList.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
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

    bool store_set_cookie_headers() const { return m_store_set_cookie_headers; }
    void set_store_set_cookie_headers(bool store_set_cookie_headers) { m_store_set_cookie_headers = store_set_cookie_headers; }

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
    bool m_store_set_cookie_headers { true };
};

}
