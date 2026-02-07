/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibHTTP/Forward.h>
#include <LibWeb/Bindings/CookieStorePrototype.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Export.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::CookieStore {

// https://cookiestore.spec.whatwg.org/#dictdef-cookielistitem
struct CookieListItem {
    Optional<String> name;
    Optional<String> value;
};

// https://cookiestore.spec.whatwg.org/#dictdef-cookiestoregetoptions
struct CookieStoreGetOptions {
    Optional<String> name;
    Optional<String> url;
};

// https://cookiestore.spec.whatwg.org/#dictdef-cookieinit
struct CookieInit {
    String name;
    String value;
    Optional<HighResolutionTime::DOMHighResTimeStamp> expires;
    Optional<String> domain;
    String path;
    Bindings::CookieSameSite same_site;
    bool partitioned { false };
};

// https://cookiestore.spec.whatwg.org/#dictdef-cookiestoredeleteoptions
struct CookieStoreDeleteOptions {
    String name;
    Optional<String> domain;
    String path;
    bool partitioned { false };
};

// https://cookiestore.spec.whatwg.org/#cookiestore
class WEB_API CookieStore final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(CookieStore, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(CookieStore);

public:
    GC::Ref<WebIDL::Promise> get(String name);
    GC::Ref<WebIDL::Promise> get(CookieStoreGetOptions const&);

    GC::Ref<WebIDL::Promise> get_all(String name);
    GC::Ref<WebIDL::Promise> get_all(CookieStoreGetOptions const&);

    GC::Ref<WebIDL::Promise> set(String name, String value);
    GC::Ref<WebIDL::Promise> set(CookieInit const&);

    GC::Ref<WebIDL::Promise> delete_(String name);
    GC::Ref<WebIDL::Promise> delete_(CookieStoreDeleteOptions const&);

    void set_onchange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onchange();

    void process_cookie_changes(Vector<HTTP::Cookie::Cookie>);

private:
    CookieStore(JS::Realm&, PageClient&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<PageClient> m_client;
};

}

namespace Web::Bindings {

JS::Value cookie_list_item_to_value(JS::Realm&, CookieStore::CookieListItem const&);

}
