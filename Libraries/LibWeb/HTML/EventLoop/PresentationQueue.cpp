/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/HTML/EventLoop/PresentationQueue.h>
#include <LibWeb/HTML/LocalNavigable.h>

namespace Web::HTML {

void PresentationQueue::submit(LocalNavigable& navigable, Compositor::CompositorFrame frame)
{
    m_entries.append({ navigable, move(frame) });
    present_ready_frames();
}

void PresentationQueue::enqueue_recording_in_flight(LocalNavigable& navigable)
{
    m_entries.append({ navigable, {} });
}

void PresentationQueue::recording_landed(LocalNavigable& navigable, Optional<Compositor::CompositorFrame> frame)
{
    auto index = m_entries.find_first_index_if([&](auto const& entry) { return entry.navigable.ptr() == &navigable && entry.in_flight(); });
    VERIFY(index.has_value());
    if (frame.has_value())
        m_entries[*index].frame = frame.release_value();
    else
        m_entries.remove(*index);
    present_ready_frames();
}

void PresentationQueue::present_landed_frames()
{
    // A recording that has landed hands its frame back here as it is taken in, which presents the frames that are ready
    // and so moves the entries: the walk starts over.
    for (size_t index = 0; index < m_entries.size();) {
        if (m_entries[index].in_flight() && m_entries[index].navigable->take_recording_in_flight_in(LocalNavigable::TakeIn::IfFinished))
            index = 0;
        else
            ++index;
    }
}

void PresentationQueue::present_all()
{
    // A frame that is ready waits only for a recording in flight, so none is left once every recording has landed.
    while (!m_entries.is_empty())
        m_entries.first_matching([](auto const& entry) { return entry.in_flight(); })->navigable->take_recording_in_flight_in(LocalNavigable::TakeIn::Wait);
}

bool PresentationQueue::goes_with_recording_in_flight(LocalNavigable const& navigable) const
{
    for (auto container = navigable.parent(); container; container = container->parent()) {
        if (any_of(m_entries, [&](auto const& entry) { return entry.navigable.ptr() == container.ptr() && entry.in_flight(); }))
            return true;
    }
    return false;
}

void PresentationQueue::present_ready_frames()
{
    for (size_t index = 0; index < m_entries.size();) {
        if (m_entries[index].in_flight() || goes_with_recording_in_flight(m_entries[index].navigable)) {
            ++index;
            continue;
        }
        auto entry = m_entries.take(index);
        // A navigable that was destroyed since it painted the frame has no compositor context to present it to.
        if (entry.navigable->has_compositor_context())
            entry.navigable->compositor_context().submit_frame(Compositor::PresentationTurn {}, entry.frame.release_value());
    }
}

void PresentationQueue::release_held_recordings_for_testing()
{
    for (auto& entry : m_entries) {
        if (entry.in_flight())
            entry.navigable->release_recording_in_flight_for_testing();
    }
}

void PresentationQueue::visit_edges(GC::Cell::Visitor& visitor)
{
    for (auto& entry : m_entries)
        visitor.visit(entry.navigable);
}

}
