/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/Time.h>
#include <LibCore/ElapsedTimer.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Page/Page.h>

namespace Web {

class LoadRequest {
public:
    LoadRequest();

    static LoadRequest create_for_url_on_page(const URL::URL& url, Page* page);

    // The main resource is the file being displayed in a frame (unlike subresources like images, scripts, etc.)
    // If a main resource fails with an HTTP error, we may still display its content if non-empty, e.g a custom 404 page.
    bool is_main_resource() const { return m_main_resource; }
    void set_main_resource(bool b) { m_main_resource = b; }

    bool is_valid() const { return m_url.is_valid(); }

    int id() const { return m_id; }

    const URL::URL& url() const { return m_url; }
    void set_url(const URL::URL& url) { m_url = url; }

    ByteString const& method() const { return m_method; }
    void set_method(ByteString const& method) { m_method = method; }

    ByteBuffer const& body() const { return m_body; }
    void set_body(ByteBuffer body) { m_body = move(body); }

    void start_timer() { m_load_timer.start(); }
    AK::Duration load_time() const { return m_load_timer.elapsed_time(); }

    JS::GCPtr<Page> page() const { return m_page.ptr(); }
    void set_page(Page& page) { m_page = page; }

    unsigned hash() const
    {
        auto body_hash = string_hash((char const*)m_body.data(), m_body.size());
        auto body_and_headers_hash = pair_int_hash(body_hash, m_headers.hash());
        auto url_and_method_hash = pair_int_hash(m_url.to_byte_string().hash(), m_method.hash());
        return pair_int_hash(body_and_headers_hash, url_and_method_hash);
    }

    bool operator==(LoadRequest const& other) const
    {
        if (m_headers.size() != other.m_headers.size())
            return false;
        for (auto const& it : m_headers) {
            auto jt = other.m_headers.find(it.key);
            if (jt == other.m_headers.end())
                return false;
            if (it.value != jt->value)
                return false;
        }
        return m_url == other.m_url && m_method == other.m_method && m_body == other.m_body;
    }

    void set_header(ByteString const& name, ByteString const& value) { m_headers.set(name, value); }
    ByteString header(ByteString const& name) const { return m_headers.get(name).value_or({}); }

    HashMap<ByteString, ByteString, CaseInsensitiveStringTraits> const& headers() const { return m_headers; }

private:
    int m_id { 0 };
    URL::URL m_url;
    ByteString m_method { "GET" };
    HashMap<ByteString, ByteString, CaseInsensitiveStringTraits> m_headers;
    ByteBuffer m_body;
    Core::ElapsedTimer m_load_timer;
    JS::Handle<Page> m_page;
    bool m_main_resource { false };
};

}

namespace AK {

template<>
struct Traits<Web::LoadRequest> : public DefaultTraits<Web::LoadRequest> {
    static unsigned hash(Web::LoadRequest const& request) { return request.hash(); }
};

}
