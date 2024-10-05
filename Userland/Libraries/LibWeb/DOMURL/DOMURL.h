/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
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
    JS_DECLARE_ALLOCATOR(DOMURL);

public:
    [[nodiscard]] static JS::NonnullGCPtr<DOMURL> create(JS::Realm&, URL::URL, JS::NonnullGCPtr<URLSearchParams> query);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMURL>> construct_impl(JS::Realm&, String const& url, Optional<String> const& base = {});

    virtual ~DOMURL() override;

    static WebIDL::ExceptionOr<String> create_object_url(JS::VM&, JS::NonnullGCPtr<FileAPI::Blob> object);
    static WebIDL::ExceptionOr<void> revoke_object_url(JS::VM&, StringView url);

    static JS::GCPtr<DOMURL> parse_for_bindings(JS::VM&, String const& url, Optional<String> const& base = {});
    static bool can_parse(JS::VM&, String const& url, Optional<String> const& base = {});

    WebIDL::ExceptionOr<String> href() const;
    WebIDL::ExceptionOr<void> set_href(String const&);

    WebIDL::ExceptionOr<String> origin() const;

    WebIDL::ExceptionOr<String> protocol() const;
    WebIDL::ExceptionOr<void> set_protocol(String const&);

    String const& username() const;
    void set_username(String const&);

    String const& password() const;
    void set_password(String const&);

    WebIDL::ExceptionOr<String> host() const;
    void set_host(String const&);

    WebIDL::ExceptionOr<String> hostname() const;
    void set_hostname(String const&);

    WebIDL::ExceptionOr<String> port() const;
    void set_port(String const&);

    String pathname() const;
    void set_pathname(String const&);

    Optional<String> const& fragment() const { return m_url.fragment(); }

    ByteString path_segment_at_index(size_t index) const { return m_url.path_segment_at_index(index); }

    void set_paths(Vector<ByteString> const& paths) { return m_url.set_paths(paths); }

    // FIXME: Reimplement this to meet the definition in https://url.spec.whatwg.org/#url-opaque-path once we modernize URL to meet the spec.
    bool cannot_be_a_base_url() const { return m_url.cannot_be_a_base_url(); }

    WebIDL::ExceptionOr<String> search() const;
    void set_search(String const&);

    JS::NonnullGCPtr<URLSearchParams const> search_params() const;

    WebIDL::ExceptionOr<String> hash() const;
    void set_hash(String const&);

    WebIDL::ExceptionOr<String> to_json() const;

    Optional<String> const& query() const { return m_url.query(); }
    void set_query(Badge<URLSearchParams>, Optional<String> query) { m_url.set_query(move(query)); }

private:
    DOMURL(JS::Realm&, URL::URL, JS::NonnullGCPtr<URLSearchParams> query);

    static JS::NonnullGCPtr<DOMURL> initialize_a_url(JS::Realm&, URL::URL const&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    URL::URL m_url;
    JS::NonnullGCPtr<URLSearchParams> m_query;
};

bool host_is_domain(URL::Host const&);

// https://url.spec.whatwg.org/#potentially-strip-trailing-spaces-from-an-opaque-path
void strip_trailing_spaces_from_an_opaque_path(DOMURL& url);

// https://url.spec.whatwg.org/#concept-url-parser
URL::URL parse(StringView input, Optional<URL::URL> const& base_url = {}, Optional<StringView> encoding = {});

}
