/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/PictureInPicture/PictureInPictureEvent.h>

namespace Web::PictureInPicture {

GC_DEFINE_ALLOCATOR(PictureInPictureEvent);

GC::Ref<PictureInPictureEvent> PictureInPictureEvent::create(Utf16FlyString const& event_name, PictureInPictureEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<PictureInPictureEvent>(event_name, event_init, time_stamp);
}

PictureInPictureEvent::PictureInPictureEvent(Utf16FlyString const& event_name, PictureInPictureEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_picture_in_picture_window(event_init.picture_in_picture_window)
{
}

PictureInPictureEvent::~PictureInPictureEvent() = default;

void PictureInPictureEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_picture_in_picture_window);
}

}
