/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <LibGC/Function.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibHTTP/Forward.h>
#include <LibJS/Forward.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/CookieStore.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CookieStore {

using CookieInit = Bindings::CookieInit;
using CookieListItem = Bindings::CookieListItem;
using CookieStoreDeleteOptions = Bindings::CookieStoreDeleteOptions;
using CookieStoreGetOptions = Bindings::CookieStoreGetOptions;

using CookieListCompletionSteps = GC::Function<void(Vector<CookieListItem>)>;
using CookieMutationCompletionSteps = GC::Function<void(bool)>;

// https://cookiestore.spec.whatwg.org/#cookiestore
class WEB_API CookieStore final : public DOM::EventTarget {
    WEB_WRAPPABLE(CookieStore, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(CookieStore);

public:
    GC::Ref<WebIDL::Promise> get(JS::Realm&, CookieStoreGetOptions const&);
    GC::Ref<WebIDL::Promise> get(JS::Realm&, String);
    void get(URL::URL, Optional<String>, GC::Ref<CookieListCompletionSteps>);

    GC::Ref<WebIDL::Promise> get_all(JS::Realm&, CookieStoreGetOptions const&);
    GC::Ref<WebIDL::Promise> get_all(JS::Realm&, String);
    void get_all(URL::URL, Optional<String>, GC::Ref<CookieListCompletionSteps>);

    GC::Ref<WebIDL::Promise> set(JS::Realm&, CookieInit const&);
    GC::Ref<WebIDL::Promise> set(JS::Realm&, String name, String value);
    void set(URL::URL, CookieInit const&, GC::Ref<CookieMutationCompletionSteps>);

    GC::Ref<WebIDL::Promise> delete_(JS::Realm&, CookieStoreDeleteOptions const&);
    GC::Ref<WebIDL::Promise> delete_(JS::Realm&, String);
    void delete_(URL::URL, CookieStoreDeleteOptions const&, GC::Ref<CookieMutationCompletionSteps>);

    void set_onchange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onchange();

    void process_cookie_changes(JS::Object&, Vector<HTTP::Cookie::Cookie>);

private:
    CookieStore(PageClient&);
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<PageClient> m_client;
};

}
