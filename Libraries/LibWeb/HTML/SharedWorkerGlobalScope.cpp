/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Diagnostics.h>
#include <LibWeb/HTML/SharedWorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SharedWorkerGlobalScope);

HashTable<GC::RawRef<SharedWorkerGlobalScope>>& all_shared_worker_global_scopes()
{
    // NB: Free what this thread kept when it exits.
#ifdef AK_COMPILER_CLANG
    AK_IGNORE_DIAGNOSTIC("-Wexit-time-destructors", static thread_local HashTable<GC::RawRef<SharedWorkerGlobalScope>> set)
#else
    static thread_local HashTable<GC::RawRef<SharedWorkerGlobalScope>> set;
#endif
    return set;
}

SharedWorkerGlobalScope::SharedWorkerGlobalScope(GC::Ref<Web::Page> page)
    : WorkerGlobalScope(page)
{
    all_shared_worker_global_scopes().set(*this);
}

SharedWorkerGlobalScope::~SharedWorkerGlobalScope() = default;

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

    // NB: This script can run on for as long as it wants to. The UI process has to know by the time it returns, as a
    //     constructor that runs after that may not pick a global whose closing flag is true.
    page()->client().page_did_close_shared_worker();
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
