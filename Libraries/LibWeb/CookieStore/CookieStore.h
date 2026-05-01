/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibHTTP/Forward.h>
#include <LibWeb/Bindings/CookieStore.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Export.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::CookieStore {

// https://cookiestore.spec.whatwg.org/#cookiestore
class WEB_API CookieStore final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(CookieStore, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(CookieStore);

public:
    GC::Ref<WebIDL::Promise> get(String name);
    GC::Ref<WebIDL::Promise> get(Bindings::CookieStoreGetOptions const&);

    GC::Ref<WebIDL::Promise> get_all(String name);
    GC::Ref<WebIDL::Promise> get_all(Bindings::CookieStoreGetOptions const&);

    GC::Ref<WebIDL::Promise> set(String name, String value);
    GC::Ref<WebIDL::Promise> set(Bindings::CookieInit const&);

    GC::Ref<WebIDL::Promise> delete_(String name);
    GC::Ref<WebIDL::Promise> delete_(Bindings::CookieStoreDeleteOptions const&);

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

JS::Value cookie_list_item_to_value(JS::Realm&, CookieListItem const&);

}
