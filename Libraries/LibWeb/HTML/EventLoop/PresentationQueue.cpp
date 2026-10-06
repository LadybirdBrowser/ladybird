/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/HTML/EventLoop/PresentationQueue.h>
#include <LibWeb/HTML/LocalNavigable.h>

namespace Web::HTML {

void PresentationQueue::submit(LocalNavigable& navigable, Compositor::SealedFrame frame)
{
    // A navigable that was destroyed since it painted the frame has no compositor context to present it to.
    if (navigable.has_compositor_context())
        navigable.compositor_context().present_sealed_frame(Compositor::PresentationTurn {}, navigable.presenter(), move(frame));
}

void PresentationQueue::submit(LocalNavigable& navigable, Compositor::CompositorFrame frame)
{
    if (navigable.has_compositor_context())
        navigable.compositor_context().submit_frame(Compositor::PresentationTurn {}, move(frame));
}

void PresentationQueue::enqueue_recording_in_flight(LocalNavigable& navigable)
{
    m_recordings_in_flight.append(navigable);
}

void PresentationQueue::recording_landed(LocalNavigable& navigable, Optional<Compositor::SealedFrame> frame)
{
    auto index = m_recordings_in_flight.find_first_index_if([&](auto const& entry) { return entry.ptr() == &navigable; });
    VERIFY(index.has_value());
    m_recordings_in_flight.remove(*index);
    if (frame.has_value())
        submit(navigable, frame.release_value());
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
