/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/RustFFI.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(IDLEventListener);

GC::Ref<IDLEventListener> IDLEventListener::create(GC::Ref<WebIDL::CallbackType> callback)
{
    return GC::Heap::the().allocate<IDLEventListener>(move(callback));
}

IDLEventListener::IDLEventListener(GC::Ref<WebIDL::CallbackType> callback)
    : m_callback(move(callback))
{
    // NB: libweb_rust compares the callbacks of event listeners by the object of their CallbackType, which it reads at
    //     these offsets (see Rust/src/dom/event_listeners.rs).
    static_assert(offsetof(IDLEventListener, m_callback) == RustFFI::IDL_EVENT_LISTENER_CALLBACK_OFFSET);
    static_assert(sizeof(GC::Ref<WebIDL::CallbackType>) == sizeof(void*));
    static_assert(offsetof(WebIDL::CallbackType, callback) == RustFFI::CALLBACK_TYPE_CALLBACK_OFFSET);
    static_assert(sizeof(GC::NonnullMember<JS::Object>) == 4);

    // NB: The fields libweb_rust reads start after all of the base class, where every C++ ABI places them, and not in
    //     its tail padding, where only the Itanium ABI could.
    static_assert(offsetof(IDLEventListener, m_callback) >= sizeof(GC::Cell));
    static_assert(offsetof(WebIDL::CallbackType, callback) >= sizeof(JS::Cell));
}

void IDLEventListener::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
}

}
