/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWeb/Bindings/CookieStorePrototype.h>
#include <LibWeb/DOM/EventTarget.h>

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

// https://cookiestore.spec.whatwg.org/#cookiestore
class CookieStore final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(CookieStore, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(CookieStore);

public:
    GC::Ref<WebIDL::Promise> get(String name);
    GC::Ref<WebIDL::Promise> get(CookieStoreGetOptions const&);

    GC::Ref<WebIDL::Promise> get_all(String name);
    GC::Ref<WebIDL::Promise> get_all(CookieStoreGetOptions const&);

    GC::Ref<WebIDL::Promise> set(String name, String value);

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
