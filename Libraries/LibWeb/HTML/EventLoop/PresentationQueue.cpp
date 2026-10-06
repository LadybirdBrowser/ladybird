/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/HTML/EventLoop/PresentationQueue.h>
#include <LibWeb/HTML/LocalNavigable.h>

namespace Web::HTML {

void PresentationQueue::enqueue_recording_in_flight(LocalNavigable& navigable)
{
    m_recordings_in_flight.append(navigable);
}

void PresentationQueue::recording_landed(LocalNavigable& navigable)
{
    auto index = m_recordings_in_flight.find_first_index_if([&](auto const& entry) { return entry.ptr() == &navigable; });
    VERIFY(index.has_value());
    m_recordings_in_flight.remove(*index);
}

void PresentationQueue::present_landed_frames()
{
    // A recording that has landed leaves the queue as it is taken in, which moves only the entries after it.
    for (size_t index = m_recordings_in_flight.size(); index-- > 0;)
        (void)m_recordings_in_flight[index]->take_recording_in_flight_in(LocalNavigable::TakeIn::IfFinished);
}

void PresentationQueue::present_all()
{
    while (!m_recordings_in_flight.is_empty())
        (void)m_recordings_in_flight.last()->take_recording_in_flight_in(LocalNavigable::TakeIn::Wait);
}

void PresentationQueue::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_recordings_in_flight);
}

}
