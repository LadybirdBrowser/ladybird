/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Clipboard/ClipboardEvent.h>

namespace Web::Clipboard {

GC_DEFINE_ALLOCATOR(ClipboardEvent);

GC::Ref<ClipboardEvent> ClipboardEvent::create(FlyString const& event_name, ClipboardEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<ClipboardEvent>(event_name, event_init, time_stamp);
}

ClipboardEvent::ClipboardEvent(FlyString const& event_name, ClipboardEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
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
