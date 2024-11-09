/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/ClipboardEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Clipboard/ClipboardEvent.h>

namespace Web::Clipboard {

JS_DEFINE_ALLOCATOR(ClipboardEvent);

JS::NonnullGCPtr<ClipboardEvent> ClipboardEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, ClipboardEventInit const& event_init)
{
    return realm.heap().allocate<ClipboardEvent>(realm, realm, event_name, event_init);
}

ClipboardEvent::ClipboardEvent(JS::Realm& realm, FlyString const& event_name, ClipboardEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_clipboard_data(event_init.clipboard_data)
{
}

ClipboardEvent::~ClipboardEvent() = default;

void ClipboardEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ClipboardEvent);
}

void ClipboardEvent::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_clipboard_data);
}

}
