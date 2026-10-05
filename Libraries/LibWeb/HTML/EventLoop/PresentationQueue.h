/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Compositor/CompositorFrame.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// The frames of the event loop's navigables on their way to the compositor, in the order step 22 of the rendering update
// paints them in. Each is presented as soon as it is ready, but for the frame of a nested navigable whose container's
// recording is still in flight: it goes along with the frame of the container that composes it, ahead of it. A frame
// does not wait for the frame of any other navigable. Only the queue hands a frame its turn
// (Compositor::PresentationTurn), so a frame presented ahead of its container's recording in flight does not compile.
class PresentationQueue {
    AK_ALLOC_WITH_KMALLOC;
    AK_MAKE_NONCOPYABLE(PresentationQueue);
    AK_MAKE_NONMOVABLE(PresentationQueue);

public:
    PresentationQueue() = default;

    // A frame recorded in step, presented once no recording of a container of its navigable is in flight.
    void submit(LocalNavigable&, Compositor::CompositorFrame);
    // A recording that flies beside the event loop, whose frame is presented in its place once it lands.
    void enqueue_recording_in_flight(LocalNavigable&);
    // The recording in flight of the navigable has landed, with the frame it finished where it still stands.
    void recording_landed(LocalNavigable&, Optional<Compositor::CompositorFrame>);

    // Between two tasks: takes in the recordings that have landed, and presents the frames that are ready.
    void present_landed_frames();
    // Presents every frame, waiting for each recording in flight to land.
    void present_all();

    // Frames wait only for a recording in flight.
    bool has_recording_in_flight() const { return !m_entries.is_empty(); }

    void visit_edges(GC::Cell::Visitor&);

private:
    bool goes_with_recording_in_flight(LocalNavigable const&) const;
    void present_ready_frames();

    struct Entry {
        bool in_flight() const { return !frame.has_value(); }

        GC::Ref<LocalNavigable> navigable;
        // None while the navigable's recording flies.
        Optional<Compositor::CompositorFrame> frame;
    };
    Vector<Entry> m_entries;
};

}
