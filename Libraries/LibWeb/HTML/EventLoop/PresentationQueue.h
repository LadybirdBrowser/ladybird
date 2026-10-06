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
#include <LibWeb/Compositor/NavigablePresenter.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// The frames the event loop's navigables seal, on their way to the Paint thread, which builds and presents each after
// the frames handed to it before, and the recordings that fly beside the event loop, which present their frames there
// themselves. Only the queue hands a frame its turn (Compositor::PresentationTurn).
class PresentationQueue {
    AK_ALLOC_WITH_KMALLOC;
    AK_MAKE_NONCOPYABLE(PresentationQueue);
    AK_MAKE_NONMOVABLE(PresentationQueue);

public:
    PresentationQueue() = default;

    // A frame sealed in step.
    void submit(LocalNavigable&, Compositor::SealedFrame);
    // A frame built by hand, for a test.
    void submit(LocalNavigable&, Compositor::CompositorFrame);
    // A recording that flies beside the event loop, whose frame is presented in its place once it lands.
    void enqueue_recording_in_flight(LocalNavigable&);
    // The recording in flight of the navigable has landed, with the frame it sealed where it still stands.
    void recording_landed(LocalNavigable&, Optional<Compositor::SealedFrame>);

    // Between two tasks: takes in the recordings that have landed.
    void present_landed_frames();
    // Waits for each recording in flight to land.
    void present_all();

    bool has_recording_in_flight() const { return !m_recordings_in_flight.is_empty(); }

    void visit_edges(GC::Cell::Visitor&);

private:
    Vector<GC::Ref<LocalNavigable>> m_recordings_in_flight;
};

}
