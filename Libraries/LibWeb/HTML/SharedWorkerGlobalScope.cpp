/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/SharedWorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SharedWorkerGlobalScope);

SharedWorkerGlobalScope::SharedWorkerGlobalScope(GC::Ref<Web::Page> page)
    : WorkerGlobalScope(page)
{
}

SharedWorkerGlobalScope::~SharedWorkerGlobalScope() = default;

void SharedWorkerGlobalScope::finalize()
{
    Base::finalize();
    WindowOrWorkerGlobalScopeMixin::finalize();
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
