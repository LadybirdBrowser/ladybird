/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/ClipboardEvent.h>
#include <LibWeb/Clipboard/ClipboardEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::Clipboard {

GC_DEFINE_ALLOCATOR(ClipboardEvent);

GC::Ref<ClipboardEvent> ClipboardEvent::construct_impl(HTML::Window& window, FlyString const& event_name, Bindings::ClipboardEventInit const& event_init)
{
    return GC::Heap::the().allocate<ClipboardEvent>(event_name, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window)));
}

ClipboardEvent::ClipboardEvent(FlyString const& event_name, Bindings::ClipboardEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_clipboard_data(event_init.clipboard_data)
{
}

ClipboardEvent::~ClipboardEvent() = default;

void ClipboardEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_clipboard_data);
}

}
