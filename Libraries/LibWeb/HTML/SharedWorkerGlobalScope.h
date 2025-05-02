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

    String const& name() const { return m_name; }

    void close();

#define __ENUMERATE(attribute_name, event_name)       \
    void set_##attribute_name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* attribute_name();
    ENUMERATE_SHARED_WORKER_GLOBAL_SCOPE_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

private:
    SharedWorkerGlobalScope(JS::Realm&, GC::Ref<Web::Page>, String name);

    virtual void initialize_web_interfaces_impl() override;
    virtual void finalize() override;

    String m_name;
};

HashTable<GC::RawRef<SharedWorkerGlobalScope>>& all_shared_worker_global_scopes();

}
