/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SharedWorkerExposedInterfaces.h>
#include <LibWeb/HTML/SharedWorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SharedWorkerGlobalScope);

HashTable<GC::RawRef<SharedWorkerGlobalScope>>& all_shared_worker_global_scopes()
{
    static HashTable<GC::RawRef<SharedWorkerGlobalScope>> set;
    return set;
}

SharedWorkerGlobalScope::SharedWorkerGlobalScope(JS::Realm& realm, GC::Ref<Web::Page> page, String name)
    : WorkerGlobalScope(realm, page)
    , m_name(move(name))
{
    all_shared_worker_global_scopes().set(*this);
}

SharedWorkerGlobalScope::~SharedWorkerGlobalScope() = default;

void SharedWorkerGlobalScope::initialize_web_interfaces_impl()
{
    auto& realm = this->realm();

    Bindings::add_shared_worker_exposed_interfaces(*this);

    SharedWorkerGlobalScopeGlobalMixin::initialize(realm, *this);
    Base::initialize_web_interfaces_impl();
}

void SharedWorkerGlobalScope::finalize()
{
    Base::finalize();
    WindowOrWorkerGlobalScopeMixin::finalize();

    all_shared_worker_global_scopes().remove(*this);
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-sharedworkerglobalscope-close
void SharedWorkerGlobalScope::close()
{
    // The close() method steps are to close a worker given this.
    close_a_worker();
}

#define __ENUMERATE(attribute_name, event_name)                                     \
    void SharedWorkerGlobalScope::set_##attribute_name(WebIDL::CallbackType* value) \
    {                                                                               \
        set_event_handler_attribute(event_name, move(value));                       \
    }                                                                               \
                                                                                    \
    WebIDL::CallbackType* SharedWorkerGlobalScope::attribute_name()                 \
    {                                                                               \
        return event_handler_attribute(event_name);                                 \
    }
ENUMERATE_SHARED_WORKER_GLOBAL_SCOPE_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

}
