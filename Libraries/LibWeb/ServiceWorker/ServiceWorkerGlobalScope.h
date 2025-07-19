/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Cell.h>
#include <LibWeb/HTML/CookieStore.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#serviceworkerglobalscope
class ServiceWorkerGlobalScope : public HTML::WorkerGlobalScope {
    WEB_PLATFORM_OBJECT(ServiceWorkerGlobalScope, HTML::WorkerGlobalScope);
    GC_DECLARE_ALLOCATOR(ServiceWorkerGlobalScope);

public:
    virtual ~ServiceWorkerGlobalScope() override;

    void set_oninstall(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> oninstall();

    void set_onactivate(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onactivate();

    void set_onfetch(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onfetch();

    void set_onmessage(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onmessage();

    void set_onmessageerror(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onmessageerror();

    GC::Ref<HTML::CookieStore> cookie_store();

protected:
    explicit ServiceWorkerGlobalScope(JS::Realm&, GC::Ref<Web::Page>);

private:
    virtual void visit_edges(Cell::Visitor& visitor) override;
    // https://wicg.github.io/cookie-store/#serviceworkerglobalscope-associated-cookiestore
    GC::Ptr<HTML::CookieStore> m_cookie_store;
};

}
