/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/SharedWorkerGlobalScopeGlobalMixin.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>

namespace Web::HTML {

#define ENUMERATE_SHARED_WORKER_GLOBAL_SCOPE_EVENT_HANDLERS(E) \
    E(onconnect, HTML::EventNames::connect)

class SharedWorkerGlobalScope
    : public WorkerGlobalScope
    , public Bindings::SharedWorkerGlobalScopeGlobalMixin {
    WEB_PLATFORM_OBJECT(SharedWorkerGlobalScope, WorkerGlobalScope);
    GC_DECLARE_ALLOCATOR(SharedWorkerGlobalScope);

public:
    virtual ~SharedWorkerGlobalScope() override;

    void set_constructor_origin(URL::Origin origin) { m_constructor_origin = move(origin); }
    URL::Origin const& constructor_origin() const { return m_constructor_origin; }

    void set_constructor_url(URL::URL url) { m_constructor_url = move(url); }
    URL::URL const& constructor_url() const { return m_constructor_url; }

    Fetch::Infrastructure::Request::CredentialsMode credentials() const { return m_credentials; }
    void set_credentials(Fetch::Infrastructure::Request::CredentialsMode credentials) { m_credentials = credentials; }

    void close();

#define __ENUMERATE(attribute_name, event_name)       \
    void set_##attribute_name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* attribute_name();
    ENUMERATE_SHARED_WORKER_GLOBAL_SCOPE_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

private:
    SharedWorkerGlobalScope(JS::Realm&, GC::Ref<Web::Page>);

    virtual void initialize_web_interfaces_impl() override;
    virtual void finalize() override;

    URL::Origin m_constructor_origin;
    URL::URL m_constructor_url;
    Fetch::Infrastructure::Request::CredentialsMode m_credentials;
};

HashTable<GC::RawRef<SharedWorkerGlobalScope>>& all_shared_worker_global_scopes();

}
