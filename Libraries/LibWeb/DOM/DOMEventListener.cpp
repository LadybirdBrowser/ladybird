/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/DOM/DOMEventListener.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/RustFFI.h>

namespace Web::DOM {

#if ARCH(X86_64)
static_assert(sizeof(DOMEventListener) == 32);
#endif

// NB: libweb_rust reads these fields in place (see Rust/src/dom/event_listeners.rs), and sets the removed flag.
static_assert(GC::Cell::embedder_flags_offset() == RustFFI::EVENT_LISTENER_FLAGS_OFFSET);
static_assert(offsetof(DOMEventListener, callback) == RustFFI::EVENT_LISTENER_CALLBACK_OFFSET);
static_assert(offsetof(DOMEventListener, type) == RustFFI::EVENT_LISTENER_TYPE_OFFSET);
static_assert(offsetof(DOMEventListener, callback) >= sizeof(JS::Cell));
static_assert(sizeof(GC::Member<IDLEventListener>) == 4 && sizeof(Utf16FlyString) == 8);

GC_DEFINE_ALLOCATOR(DOMEventListener);

DOMEventListener::DOMEventListener() = default;
DOMEventListener::~DOMEventListener() = default;

void DOMEventListener::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(callback);
    visitor.visit(signal);
}

}
