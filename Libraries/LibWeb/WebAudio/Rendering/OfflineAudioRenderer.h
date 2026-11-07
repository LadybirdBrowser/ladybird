/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/ConditionVariable.h>
#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/ControlMessageQueue.h>
#include <LibWeb/WebAudio/Rendering/RenderGraph.h>
#include <LibWeb/WebAudio/Types.h>

namespace Core {

class EventLoop;

}

namespace Web::WebAudio::Rendering {

// Renders an OfflineAudioContext's graph on a dedicated rendering thread, which is created when rendering starts and
// exits once the last render quantum has been processed. Progress is reported back to the control thread by posting
// events to the main thread's event loop.
// https://webaudio.github.io/web-audio-api/#OfflineAudioContext
struct OfflineAudioRendererCallbacks;

class WEB_API OfflineAudioRenderer final : public AtomicRefCounted<OfflineAudioRenderer> {
public:
    static NonnullRefPtr<OfflineAudioRenderer> create(NonnullRefPtr<ControlMessageQueue>, NodeID destination_node_id, size_t number_of_channels, size_t length, float sample_rate, size_t quantum_size);

    ~OfflineAudioRenderer();

    // The callbacks below are set on the control thread before rendering starts and are always invoked on the control
    // thread. They are cleared once rendering completes to break the reference cycle with the context.
    void set_on_complete(Function<void()>);
    void set_on_suspended(Function<void(double suspend_time)>);
    void set_on_sources_ended(Function<void(Vector<NodeID> const&)>);
    void clear_callbacks();

    void start_rendering();

    // Ends rendering without completing it, unblocking the rendering thread if it is waiting in a suspension.
    void stop();

    // Registers a suspension at the given render quantum boundary. Returns false if the rendering thread already
    // looked for suspensions at that boundary.
    bool request_suspend(u64 frame);
    void resume();

    bool is_rendering_started() const;
    u64 frames_rendered() const;

    // Only safe to access on the control thread after the completion callback was invoked.
    Vector<ReadonlySpan<float>> const& rendered_channels() const { return m_rendered_channels; }

private:
    OfflineAudioRenderer(NonnullRefPtr<ControlMessageQueue>, NodeID destination_node_id, size_t number_of_channels, size_t length, float sample_rate, size_t quantum_size);

    NonnullRefPtr<OfflineAudioRendererCallbacks> m_callbacks;
    void const* m_renderer { nullptr };
    Vector<ReadonlySpan<float>> m_rendered_channels;
};

}
