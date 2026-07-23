/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/WebAudio/Rendering/OfflineAudioRenderer.h>
#include <LibWeb/WebAudio/Rendering/RenderNodes.h>

namespace Web::WebAudio::Rendering {

NonnullRefPtr<OfflineAudioRenderer> OfflineAudioRenderer::create(NonnullRefPtr<ControlMessageQueue> control_message_queue, NodeID destination_node_id, size_t number_of_channels, size_t length, float sample_rate, size_t quantum_size)
{
    return adopt_ref(*new OfflineAudioRenderer(move(control_message_queue), destination_node_id, number_of_channels, length, sample_rate, quantum_size));
}

OfflineAudioRenderer::OfflineAudioRenderer(NonnullRefPtr<ControlMessageQueue> control_message_queue, NodeID destination_node_id, size_t number_of_channels, size_t length, float sample_rate, size_t quantum_size)
    : m_control_message_queue(move(control_message_queue))
    , m_destination_node_id(destination_node_id)
    , m_number_of_channels(number_of_channels)
    , m_length(length)
    , m_sample_rate(sample_rate)
    , m_quantum_size(quantum_size)
    , m_main_thread_event_loop(Core::EventLoop::current())
{
    m_rendered_channels.resize(number_of_channels);
    for (auto& channel : m_rendered_channels)
        channel.resize(length);
}

void OfflineAudioRenderer::set_on_complete(Function<void()> on_complete)
{
    m_on_complete = move(on_complete);
}

void OfflineAudioRenderer::set_on_suspended(Function<void(double)> on_suspended)
{
    m_on_suspended = move(on_suspended);
}

void OfflineAudioRenderer::set_on_sources_ended(Function<void(Vector<NodeID> const&)> on_sources_ended)
{
    m_on_sources_ended = move(on_sources_ended);
}

void OfflineAudioRenderer::clear_callbacks()
{
    m_on_complete = nullptr;
    m_on_suspended = nullptr;
    m_on_sources_ended = nullptr;
}

void OfflineAudioRenderer::start_rendering()
{
    VERIFY(!m_thread);
    m_graph.set_destination(m_destination_node_id);
    m_thread = MUST(Threading::Thread::try_create("OfflineAudioRenderer"sv, [self = NonnullRefPtr(*this)] {
        return self->render_thread_main();
    }));
    m_thread->start();
    m_thread->detach();
}

// Ends rendering without completing it, unblocking the rendering thread if it is waiting in a suspension.
void OfflineAudioRenderer::stop()
{
    Sync::MutexLocker locker(m_suspend_mutex);
    m_shutting_down = true;
    m_resume_signal.broadcast();
}

// Registers a suspension at the given render quantum boundary. Returns false if the rendering thread already looked
// for suspensions at that boundary.
bool OfflineAudioRenderer::request_suspend(u64 frame)
{
    Sync::MutexLocker locker(m_suspend_mutex);
    if (frame < m_next_suspend_check_frame)
        return false;
    m_suspend_frames.set(frame);
    return true;
}

void OfflineAudioRenderer::resume()
{
    Sync::MutexLocker locker(m_suspend_mutex);
    m_suspended = false;
    m_resume_signal.broadcast();
}

void OfflineAudioRenderer::post_to_control_thread(Function<void(OfflineAudioRenderer&)> callback)
{
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), callback = move(callback)] {
        callback(*self);
    });
}

intptr_t OfflineAudioRenderer::render_thread_main()
{
    u64 current_frame = 0;
    while (current_frame < m_length) {
        // Suspend rendering if a suspension was scheduled at this render quantum boundary.
        {
            Sync::MutexLocker locker(m_suspend_mutex);
            if (m_shutting_down)
                return 0;
            m_next_suspend_check_frame = current_frame + m_quantum_size;
            if (m_suspend_frames.remove(current_frame)) {
                m_suspended = true;
                auto suspend_time = current_frame / static_cast<double>(m_sample_rate);
                post_to_control_thread([suspend_time](OfflineAudioRenderer& renderer) {
                    if (renderer.m_on_suspended)
                        renderer.m_on_suspended(suspend_time);
                });
                m_resume_signal.wait_while([&] { return m_suspended && !m_shutting_down; });
                if (m_shutting_down)
                    return 0;
            }
        }

        m_graph.apply_control_messages(m_control_message_queue->drain());

        RenderContext context {
            .sample_rate = m_sample_rate,
            .quantum_size = m_quantum_size,
            .quantum_start_frame = current_frame,
        };
        m_graph.render_quantum(context);

        // Copy the destination's captured output into the rendered buffer.
        if (auto* destination_node = m_graph.destination()) {
            auto const& destination_output = destination_node->output(0);
            auto frames_to_copy = min(m_quantum_size, m_length - current_frame);
            for (size_t channel_index = 0; channel_index < m_number_of_channels; ++channel_index) {
                if (channel_index >= destination_output.channel_count())
                    break;
                auto samples = destination_output.channel(channel_index);
                for (size_t frame = 0; frame < frames_to_copy; ++frame)
                    m_rendered_channels[channel_index][current_frame + frame] = samples[frame];
            }
        }

        current_frame += m_quantum_size;
        m_frames_rendered.store(current_frame);

        if (auto ended_nodes = m_graph.take_ended_nodes(); !ended_nodes.is_empty()) {
            post_to_control_thread([ended_nodes = move(ended_nodes)](OfflineAudioRenderer& renderer) {
                if (renderer.m_on_sources_ended)
                    renderer.m_on_sources_ended(ended_nodes);
            });
        }
    }

    post_to_control_thread([](OfflineAudioRenderer& renderer) {
        if (renderer.m_on_complete)
            renderer.m_on_complete();
    });
    return 0;
}

}
