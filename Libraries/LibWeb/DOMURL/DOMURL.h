/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOMURL/URLSearchParams.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOMURL {

class DOMURL : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DOMURL, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(DOMURL);

public:
    [[nodiscard]] static GC::Ref<DOMURL> create(JS::Realm&, URL::URL, GC::Ref<URLSearchParams> query);
    static WebIDL::ExceptionOr<GC::Ref<DOMURL>> construct_impl(JS::Realm&, String const& url, Optional<String> const& base = {});

    virtual ~DOMURL() override;

    static WebIDL::ExceptionOr<String> create_object_url(JS::VM&, GC::Ref<FileAPI::Blob> object);
    static void revoke_object_url(JS::VM&, StringView url);

    static GC::Ptr<DOMURL> parse_for_bindings(JS::VM&, String const& url, Optional<String> const& base = {});
    static bool can_parse(JS::VM&, String const& url, Optional<String> const& base = {});

    String href() const;
    WebIDL::ExceptionOr<void> set_href(String const&);

    String origin() const;

    String protocol() const;
    void set_protocol(String const&);

    String const& username() const;
    void set_username(String const&);

    String const& password() const;
    void set_password(String const&);

    String host() const;
    void set_host(String const&);

    String hostname() const;
    void set_hostname(String const&);

    String port() const;
    void set_port(String const&);

    String pathname() const;
    void set_pathname(String const&);

    Optional<String> const& fragment() const { return m_url.fragment(); }

    ByteString path_segment_at_index(size_t index) const { return m_url.path_segment_at_index(index); }

    void set_paths(Vector<ByteString> const& paths) { return m_url.set_paths(paths); }

    // FIXME: Reimplement this to meet the definition in https://url.spec.whatwg.org/#url-opaque-path once we modernize URL to meet the spec.
    bool cannot_be_a_base_url() const { return m_url.cannot_be_a_base_url(); }

    String search() const;
    void set_search(String const&);

    GC::Ref<URLSearchParams const> search_params() const;

    String hash() const;
    void set_hash(String const&);

    String to_json() const;

    Optional<String> const& query() const { return m_url.query(); }
    void set_query(Badge<URLSearchParams>, Optional<String> query) { m_url.set_query(move(query)); }

private:
    DOMURL(JS::Realm&, URL::URL, GC::Ref<URLSearchParams> query);

    static GC::Ref<DOMURL> initialize_a_url(JS::Realm&, URL::URL const&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    URL::URL m_url;
    GC::Ref<URLSearchParams> m_query;
};

// https://url.spec.whatwg.org/#concept-url-parser
Optional<URL::URL> parse(StringView input, Optional<URL::URL const&> base_url = {}, Optional<StringView> encoding = {});

}
