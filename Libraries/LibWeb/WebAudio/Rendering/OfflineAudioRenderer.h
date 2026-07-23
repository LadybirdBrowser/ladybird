/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
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
class WEB_API OfflineAudioRenderer final : public AtomicRefCounted<OfflineAudioRenderer> {
public:
    static NonnullRefPtr<OfflineAudioRenderer> create(NonnullRefPtr<ControlMessageQueue>, NodeID destination_node_id, size_t number_of_channels, size_t length, float sample_rate, size_t quantum_size);

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

    bool is_rendering_started() const { return m_thread != nullptr; }
    u64 frames_rendered() const { return m_frames_rendered.load(); }

    // Only safe to access on the control thread after the completion callback was invoked.
    Vector<Vector<float>> const& rendered_channels() const { return m_rendered_channels; }

private:
    OfflineAudioRenderer(NonnullRefPtr<ControlMessageQueue>, NodeID destination_node_id, size_t number_of_channels, size_t length, float sample_rate, size_t quantum_size);

    intptr_t render_thread_main();
    void post_to_control_thread(Function<void(OfflineAudioRenderer&)>);

    NonnullRefPtr<ControlMessageQueue> m_control_message_queue;
    NodeID m_destination_node_id;
    size_t m_number_of_channels { 0 };
    size_t m_length { 0 };
    float m_sample_rate { 0 };
    size_t m_quantum_size { 0 };

    Core::EventLoop& m_main_thread_event_loop;
    RefPtr<Threading::Thread> m_thread;
    RenderGraph m_graph;
    Vector<Vector<float>> m_rendered_channels;
    Atomic<u64> m_frames_rendered { 0 };

    Function<void()> m_on_complete;
    Function<void(double)> m_on_suspended;
    Function<void(Vector<NodeID> const&)> m_on_sources_ended;

    Sync::Mutex m_suspend_mutex;
    Sync::ConditionVariable m_resume_signal { m_suspend_mutex };
    HashTable<u64> m_suspend_frames;

    // The next render quantum boundary at which the rendering thread will look for a scheduled suspension. It is
    // published under the same lock that guards the suspension set, so a suspension can never be registered for a
    // boundary the rendering thread has already checked.
    u64 m_next_suspend_check_frame { 0 };

    bool m_suspended { false };
    bool m_shutting_down { false };
};

}
