/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/Engine/OfflineAudioRenderThread.h>
#include <LibWeb/WebAudio/RenderGraph.h>
#include <errno.h>

namespace Web::WebAudio::Render {

static void snapshot_analysers(RenderGraph const& graph, GraphDescription const& graph_description,
    HashMap<NodeID, Vector<f32>>& time_domain_out,
    HashMap<NodeID, Vector<f32>>& frequency_db_out)
{
    ASSERT_RENDER_THREAD();
    time_domain_out.clear();
    frequency_db_out.clear();

    size_t const analyser_count = graph.analyser_count();
    for (size_t analyser_index = 0; analyser_index < analyser_count; ++analyser_index) {
        NodeID const analyser_node_id = graph.analyser_node_id(analyser_index);
        auto maybe_node = graph_description.nodes.get(analyser_node_id);
        if (!maybe_node.has_value() || !maybe_node->has<AnalyserGraphNode>())
            continue;

        size_t const fft_size = maybe_node->get<AnalyserGraphNode>().fft_size;
        if (fft_size == 0)
            continue;

        Vector<f32> time_domain_data;
        time_domain_data.resize(fft_size);
        if (graph.copy_analyser_time_domain_data(analyser_index, time_domain_data.span()))
            time_domain_out.set(analyser_node_id, move(time_domain_data));

        Vector<f32> frequency_data_db;
        frequency_data_db.resize(fft_size / 2);
        if (graph.copy_analyser_frequency_data_db(analyser_index, frequency_data_db.span()))
            frequency_db_out.set(analyser_node_id, move(frequency_data_db));
    }
}

static OfflineAudioRenderResult render_offline_audio_graph(OfflineAudioRenderRequest const& request)
{
    ASSERT_RENDER_THREAD();
    OfflineAudioRenderResult result;

    u32 const channel_count = request.number_of_channels;
    u32 const length_in_frames = request.length_in_sample_frames;
    u32 const quantum_size = request.render_quantum_size > 0
        ? request.render_quantum_size
        : static_cast<u32>(RENDER_QUANTUM_SIZE);

    result.rendered_channels.resize(channel_count);
    for (u32 channel_index = 0; channel_index < channel_count; ++channel_index) {
        result.rendered_channels[channel_index].resize(length_in_frames);
        for (u32 i = 0; i < length_in_frames; ++i)
            result.rendered_channels[channel_index][i] = 0.0f;
    }

    RenderGraph graph(request.graph, request.sample_rate, request.render_quantum_size > 0 ? request.render_quantum_size : static_cast<u32>(RENDER_QUANTUM_SIZE), request.resources.ptr());

    u32 frame_index = 0;
    while (frame_index < length_in_frames) {
        // https://webaudio.github.io/web-audio-api/#render-quantum
        graph.begin_new_quantum(frame_index);

        AudioBus const& destination_bus = graph.render_destination_for_current_quantum();

        graph.render_analysers_for_current_quantum();

        u32 const frames_this_quantum = min(quantum_size, length_in_frames - frame_index);
        for (u32 out_channel = 0; out_channel < channel_count; ++out_channel) {
            Vector<f32>& output = result.rendered_channels[out_channel];
            auto bus_channel0 = destination_bus.channel(min(out_channel, destination_bus.channel_count() - 1));
            for (u32 i = 0; i < frames_this_quantum; ++i)
                output[frame_index + i] = bus_channel0[i];
        }

        frame_index += frames_this_quantum;
    }

    snapshot_analysers(graph, request.graph, result.analyser_time_domain_data, result.analyser_frequency_data_db);

    return result;
}

OfflineAudioRenderThread::OfflineAudioRenderThread(OfflineAudioRenderRequest request, CompletionDispatcher completion_dispatcher, int suspend_write_fd)
    : m_request(move(request))
    , m_completion_dispatcher(move(completion_dispatcher))
    , m_suspend_write_fd(suspend_write_fd)
    , m_thread(Threading::Thread::construct("OfflineAudioRndr"sv, [this] {
        rendering_thread_loop();
        return static_cast<intptr_t>(0);
    }))
{
}

OfflineAudioRenderThread::~OfflineAudioRenderThread()
{
    request_abort();
    {
        Threading::MutexLocker const locker { m_mutex };
        m_resume_requested = true;
        m_resume_condition.signal();
    }

    (void)m_thread->join();

    if (m_suspend_write_fd >= 0) {
        MUST(Core::System::close(m_suspend_write_fd));
        m_suspend_write_fd = -1;
    }
}

void OfflineAudioRenderThread::start()
{
    m_thread->start();
}

void OfflineAudioRenderThread::request_abort()
{
    Threading::MutexLocker const locker { m_mutex };
    if (m_abort_requested)
        return;
    m_abort_requested = true;
    m_resume_requested = true;
    m_resume_condition.signal();
}

bool OfflineAudioRenderThread::is_finished() const
{
    Threading::MutexLocker const locker { m_mutex };
    return m_finished;
}

Optional<OfflineAudioRenderResult> OfflineAudioRenderThread::take_result()
{
    Threading::MutexLocker const locker { m_mutex };
    if (!m_finished)
        return {};

    Optional<OfflineAudioRenderResult> result = move(m_result);
    m_result.clear();
    return result;
}

Optional<OfflineAudioAnalyserSnapshot> OfflineAudioRenderThread::take_analyser_snapshot(u32 expected_frame_index)
{
    Threading::MutexLocker const locker { m_mutex };
    if (!m_latest_analyser_snapshot.has_value())
        return {};

    if (m_latest_analyser_snapshot->frame_index != expected_frame_index)
        return {};

    Optional<OfflineAudioAnalyserSnapshot> snapshot = move(m_latest_analyser_snapshot);
    m_latest_analyser_snapshot.clear();
    return snapshot;
}

void OfflineAudioRenderThread::request_resume(Optional<OfflineAudioGraphUpdate> updated_graph)
{
    Threading::MutexLocker const locker { m_mutex };
    if (m_abort_requested || m_finished)
        return;

    if (updated_graph.has_value())
        m_pending_graph_update = move(updated_graph);

    m_resume_requested = true;
    m_resume_condition.signal();
}

void OfflineAudioRenderThread::signal_completion() const
{
    WA_DBGLN("[WebAudio] offline render thread signaling completion");
    if (m_completion_dispatcher)
        m_completion_dispatcher();
}

void OfflineAudioRenderThread::signal_suspended(u32 frame_index) const
{
    if (m_suspend_write_fd < 0)
        return;

    auto result = Core::System::write(m_suspend_write_fd, { &frame_index, sizeof(frame_index) });
    if (result.is_error()) {
        // If the control thread has already torn us down, just drop the signal and ignore all errors.
        return;
    }
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-startrendering
void OfflineAudioRenderThread::rendering_thread_loop()
{
    mark_current_thread_as_offline_thread();
    ASSERT_RENDER_THREAD();

    WA_DBGLN("[WebAudio] offline render thread loop start length={} sr={}", m_request.length_in_sample_frames, m_request.sample_rate);

    // NOTE: This shares realtime constraints in that it doesn't touch GC-managed WebAudio objects.
    // It operates on the RenderGraphDescription snapshot passed in via OfflineAudioRenderRequest.

    {
        Threading::MutexLocker const locker { m_mutex };
        if (m_abort_requested) {
            m_finished = true;
            signal_completion();
            return;
        }
    }

    // NOTE: render_offline_audio_graph() currently renders the full buffer without suspension.
    // If there are no suspend points scheduled, keep using the simple path.
    if (m_request.suspend_frame_indices.is_empty()) {
        OfflineAudioRenderResult result = render_offline_audio_graph(m_request);

        {
            Threading::MutexLocker const locker { m_mutex };
            if (!m_abort_requested)
                m_result = move(result);
            m_finished = true;
        }

        signal_completion();
        WA_DBGLN("[WebAudio] offline render thread loop done simple");
        return;
    }

    OfflineAudioRenderResult result;

    u32 const channel_count = m_request.number_of_channels;
    u32 const length_in_frames = m_request.length_in_sample_frames;
    u32 const quantum_size = m_request.render_quantum_size > 0
        ? m_request.render_quantum_size
        : static_cast<u32>(RENDER_QUANTUM_SIZE);

    result.rendered_channels.resize(channel_count);
    for (u32 channel_index = 0; channel_index < channel_count; ++channel_index) {
        result.rendered_channels[channel_index].resize(length_in_frames);
        result.rendered_channels[channel_index].fill(0.0f);
    }

    GraphDescription current_graph_description = m_request.graph;
    RenderGraph graph(current_graph_description, m_request.sample_rate, m_request.render_quantum_size > 0 ? m_request.render_quantum_size : static_cast<u32>(RENDER_QUANTUM_SIZE), m_request.resources.ptr());

    u32 frame_index = 0;
    size_t next_suspend_index = 0;
    while (frame_index < length_in_frames) {
        {
            Threading::MutexLocker const locker { m_mutex };
            if (m_abort_requested)
                break;
        }

        // Shall we suspend?
        if (next_suspend_index < m_request.suspend_frame_indices.size()
            && frame_index == m_request.suspend_frame_indices[next_suspend_index]) {
            // Captured analyser data reflects the most recent render quantum completed before frame_index.
            OfflineAudioAnalyserSnapshot analyser_snapshot;
            analyser_snapshot.frame_index = frame_index;
            analyser_snapshot.render_quantum_index = frame_index / quantum_size;
            snapshot_analysers(graph, current_graph_description, analyser_snapshot.analyser_time_domain_data, analyser_snapshot.analyser_frequency_data_db);

            {
                Threading::MutexLocker const locker { m_mutex };
                m_resume_requested = false;
                m_latest_analyser_snapshot = move(analyser_snapshot);
            }

            signal_suspended(frame_index);
            Optional<OfflineAudioGraphUpdate> graph_update;
            {
                Threading::MutexLocker locker { m_mutex };
                while (!m_resume_requested && !m_abort_requested) {
                    m_resume_condition.wait();
                }
                if (m_abort_requested)
                    break;

                m_resume_requested = false;
                graph_update = move(m_pending_graph_update);
                m_pending_graph_update.clear();
            }

            if (graph_update.has_value()) {
                *m_request.resources = move(graph_update->resources);
                current_graph_description = move(graph_update->graph);
                graph.apply_update_offline(current_graph_description);
            }

            ++next_suspend_index;
        }

        // https://webaudio.github.io/web-audio-api/#render-quantum
        graph.begin_new_quantum(frame_index);

        AudioBus const& destination_bus = graph.render_destination_for_current_quantum();
        graph.render_analysers_for_current_quantum();

        u32 const frames_this_quantum = min(quantum_size, length_in_frames - frame_index);
        for (u32 out_channel = 0; out_channel < channel_count; ++out_channel) {
            Vector<f32>& output = result.rendered_channels[out_channel];
            auto bus_channel0 = destination_bus.channel(min(out_channel, destination_bus.channel_count() - 1));
            for (u32 i = 0; i < frames_this_quantum; ++i)
                output[frame_index + i] = bus_channel0[i];
        }

        frame_index += frames_this_quantum;
    }

    snapshot_analysers(graph, current_graph_description, result.analyser_time_domain_data, result.analyser_frequency_data_db);

    {
        Threading::MutexLocker const locker { m_mutex };
        if (!m_abort_requested)
            m_result = move(result);
        m_finished = true;
    }

    signal_completion();
    WA_DBGLN("[WebAudio] offline render thread loop done suspendable");
}

}
