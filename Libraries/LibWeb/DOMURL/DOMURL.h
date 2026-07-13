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
#include <LibWeb/Export.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOMURL {

class DOMURL : public Bindings::PlatformObject {
    // NOTE: This is 'URL' in the IDL, but we call it DOMURL to avoid name conflicts with LibURL.
    WEB_PLATFORM_OBJECT(URL, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(DOMURL);

public:
    [[nodiscard]] static GC::Ref<DOMURL> create(JS::Realm&, URL::URL, GC::Ref<URLSearchParams> query);
    static WebIDL::ExceptionOr<GC::Ref<DOMURL>> construct_impl(JS::Realm&, Utf16String const& url, Optional<Utf16String> const& base = {});

    virtual ~DOMURL() override;

    static WebIDL::ExceptionOr<Utf16String> create_object_url(JS::VM&, FileAPI::BlobURLEntry::Object object);
    static void revoke_object_url(JS::VM&, Utf16String const& url);

    static GC::Ptr<DOMURL> parse_for_bindings(JS::VM&, Utf16String const& url, Optional<Utf16String> const& base = {});
    static bool can_parse(JS::VM&, Utf16String const& url, Optional<Utf16String> const& base = {});

    Utf16String href() const;
    WebIDL::ExceptionOr<void> set_href(Utf16String const&);

    Utf16String origin() const;

    Utf16String protocol() const;
    void set_protocol(Utf16String const&);

    Utf16String username() const;
    void set_username(Utf16String const&);

    Utf16String password() const;
    void set_password(Utf16String const&);

    Utf16String host() const;
    void set_host(Utf16String const&);

    Utf16String hostname() const;
    void set_hostname(Utf16String const&);

    Utf16String port() const;
    void set_port(Utf16String const&);

    Utf16String pathname() const;
    void set_pathname(Utf16String const&);

    Optional<String> const& fragment() const { return m_url.fragment(); }

    ByteString path_segment_at_index(size_t index) const { return m_url.path_segment_at_index(index); }

    void set_paths(Vector<ByteString> const& paths) { return m_url.set_paths(paths); }

    bool has_an_opaque_path() const { return m_url.has_an_opaque_path(); }

    Utf16String search() const;
    void set_search(Utf16String const&);

    GC::Ref<URLSearchParams const> search_params() const;

    Utf16String hash() const;
    void set_hash(Utf16String const&);

    Utf16String to_json() const;

    Optional<String> const& query() const { return m_url.query(); }
    void set_query(Badge<URLSearchParams>, Optional<String> query) { m_url.set_query(move(query)); }

    virtual Optional<URL::Origin> extract_an_origin() const override;

private:
    DOMURL(JS::Realm&, URL::URL, GC::Ref<URLSearchParams> query);

    static GC::Ref<DOMURL> initialize_a_url(JS::Realm&, URL::URL const&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    URL::URL m_url;
    GC::Ref<URLSearchParams> m_query;
};

// https://url.spec.whatwg.org/#concept-url-parser
WEB_API Optional<URL::URL> parse_from_byte_string(StringView input, Optional<URL::URL const&> base_url = {}, Optional<StringView> encoding = {});
WEB_API Optional<URL::URL> parse(Utf16View input, Optional<URL::URL const&> base_url = {}, Optional<Utf16View> encoding = {});

}
