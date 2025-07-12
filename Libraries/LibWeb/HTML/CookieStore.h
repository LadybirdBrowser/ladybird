/*
 * Copyright (c) 2025, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/CookieStorePrototype.h>
#include <LibWeb/Bindings/TextTrackPrototype.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

struct CookieInit {
    String name;
    String value;
    Optional<double> expires;
    Optional<String> domain;
    Optional<String> path;
    Bindings::CookieSameSite same_site { Bindings::CookieSameSite::None };
    bool partitioned { false };

    String to_string() const;
};

struct CookieStoreGetOptions {
    Optional<String> name;
    Optional<String> url;

    bool is_empty() const
    {
        return !name.has_value() && !url.has_value();
    }

    String to_string() const;
};

struct CookieStoreDeleteOptions {
    String name;
    Optional<String> domain {};
    String path { "/"_string };
    bool partitioned { false };

    String to_string() const;
};

struct CookieListItem {
    String name;
    String value;
    Optional<String> domain;
    String path;
    Optional<double> expires;
    bool secure { false };
    Bindings::CookieSameSite same_site { Bindings::CookieSameSite::None };
    bool partitioned { false };

    JS::Value as_js_value(JS::Realm& realm) const;
};

// https://wicg.github.io/cookie-store/#CookieStore
class CookieStore final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(CookieStore, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(CookieStore);

public:
    [[nodiscard]] static GC::Ref<CookieStore> create(JS::Realm&, GC::Ref<Page>);
    virtual ~CookieStore() override = default;

    // https://wicg.github.io/cookie-store/#dom-cookiestore-get
    GC::Ref<WebIDL::Promise> get(String const&);

    // https://wicg.github.io/cookie-store/#dom-cookiestore-get-options
    GC::Ref<WebIDL::Promise> get(Optional<CookieStoreGetOptions> const& options = {});

    // https://wicg.github.io/cookie-store/#dom-cookiestore-getall
    GC::Ref<WebIDL::Promise> get_all(String const&);

    // https://wicg.github.io/cookie-store/#dom-cookiestore-getall-options
    GC::Ref<WebIDL::Promise> get_all(Optional<CookieStoreGetOptions> const& options = {});

    // https://wicg.github.io/cookie-store/#dom-cookiestore-set
    GC::Ref<WebIDL::Promise> set(String const&, String const&);

    // https://wicg.github.io/cookie-store/#dom-cookiestore-set-options
    GC::Ref<WebIDL::Promise> set(CookieInit const&);

    // https://wicg.github.io/cookie-store/#dom-cookiestore-delete
    GC::Ref<WebIDL::Promise> delete_(String const&);

    // https://wicg.github.io/cookie-store/#dom-cookiestore-delete-options
    GC::Ref<WebIDL::Promise> delete_(CookieStoreDeleteOptions const&);

    // https://wicg.github.io/cookie-store/#query-cookies-algorithm
    Vector<CookieListItem> query_a_cookie(URL::URL const&, Optional<String>) const;

    // https://wicg.github.io/cookie-store/#set-cookie-algorithm
    bool set_a_cookie(URL::URL const&, String, String, Optional<HighResolutionTime::DOMHighResTimeStamp>, Optional<String>, Optional<String>, Bindings::CookieSameSite, bool);

    // https://wicg.github.io/cookie-store/#delete-cookie-algorithm
    bool delete_a_cookie(URL::URL const&, String, Optional<String>, Optional<String>, bool);

private:
    CookieStore(JS::Realm&, GC::Ref<Page>);

    virtual void initialize(JS::Realm&) override;
    void visit_edges(Visitor&) override;

    GC::Ref<Page> m_page;
};

}
