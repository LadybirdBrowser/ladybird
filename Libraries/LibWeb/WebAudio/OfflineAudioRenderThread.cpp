/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibWeb/WebAudio/OfflineAudioRenderThread.h>
#include <LibWeb/WebAudio/RenderGraph.h>
#include <LibWeb/WebAudio/RenderProcessContext.h>

namespace Web::WebAudio {

static bool render_a_quantum(RenderGraph& graph,
    OfflineAudioRenderResult& out,
    u32 current_frame,
    u32 frames_this_quantum,
    u32 channel_count)
{
    // The following steps MUST be performed when rendering a render quantum.
    // https://webaudio.github.io/web-audio-api/#render-quantum

    // 1. Let render result be false.
    bool render_result = false;

    // FIXME: 2. Process the control message queue.
    // FIXME: 3. Process the BaseAudioContext’s associated task queue.

    // 4. Process a render quantum.
    // FIXME: 4.1. If the [[rendering thread state]] of the BaseAudioContext is not running, return false.

    graph.begin_quantum(current_frame);
    AudioBus const& destination_bus = graph.render_destination_for_current_quantum();

    VERIFY(destination_bus.channel_count() > 0);

    for (u32 out_channel = 0; out_channel < channel_count; ++out_channel) {
        auto bus_channel = destination_bus.channel(min(out_channel, destination_bus.channel_count() - 1));
        auto& output = out.rendered_channels[out_channel];

        bus_channel.slice(0, frames_this_quantum)
            .copy_to(output.span().slice(current_frame, frames_this_quantum));
    }

    // FIXME: 4.5. Atomically perform the following steps:
    // 4.5.1. Increment [[current frame]] by the render quantum size.
    // NB: Incrementing current_frame is handled by render_offline_audio_graph.
    // FIXME: 4.5.2. Set currentTime to [[current frame]] divided by sampleRate.

    // 4.6. Set render result to true.
    render_result = true;

    // FIXME: 5. Perform a microtask checkpoint.

    // 6. Return render result.
    return render_result;
}

// https://webaudio.github.io/web-audio-api/#rendering-loop
static OfflineAudioRenderResult render_offline_audio_graph(OfflineAudioRenderRequest const& request)
{
    OfflineAudioRenderResult result;

    u32 const channel_count = request.number_of_channels;
    u32 const length_in_frames = request.length_in_sample_frames;
    u32 const quantum_size = static_cast<u32>(RENDER_QUANTUM_SIZE);

    result.rendered_channels.resize(channel_count);
    for (u32 channel_index = 0; channel_index < channel_count; ++channel_index) {
        result.rendered_channels[channel_index].resize(length_in_frames);
        for (u32 i = 0; i < length_in_frames; ++i)
            result.rendered_channels[channel_index][i] = 0.0f;
    }

    RenderGraph graph(request.graph, request.sample_rate);
    // The following step MUST be performed once before the rendering loop starts.
    // FIXME: 1. Set the internal slot [[current frame]] of the BaseAudioContext to 0. Also set currentTime to 0.
    u32 current_frame = 0;
    while (current_frame < length_in_frames) {
        u32 frames_this_quantum = min<u32>(quantum_size, length_in_frames - current_frame);

        if (!render_a_quantum(graph, result, current_frame, frames_this_quantum, channel_count))
            break;

        current_frame += frames_this_quantum;
    }
    return result;
}

OfflineAudioRenderThread::OfflineAudioRenderThread(OfflineAudioRenderRequest request, int completion_write_fd)
    : m_request(move(request))
    , m_completion_write_fd(completion_write_fd)
    , m_thread(Threading::Thread::construct("WebAudio Offline Render"sv, [this] {
        rendering_thread_loop();
        return static_cast<intptr_t>(0);
    }))
{
}

OfflineAudioRenderThread::~OfflineAudioRenderThread()
{
    (void)m_thread->join();

    if (m_completion_write_fd >= 0) {
        MUST(Core::System::close(m_completion_write_fd));
        m_completion_write_fd = -1;
    }
}

void OfflineAudioRenderThread::start()
{
    m_thread->start();
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

void OfflineAudioRenderThread::signal_completion() const
{
    if (m_completion_write_fd < 0)
        return;

    u8 byte = 0;
    auto result = Core::System::write(m_completion_write_fd, { &byte, sizeof(byte) });
    if (result.is_error()) {
        return;
    }
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-startrendering
void OfflineAudioRenderThread::rendering_thread_loop()
{
    OfflineAudioRenderResult result = render_offline_audio_graph(m_request);

    {
        Threading::MutexLocker const locker { m_mutex };
        m_result = move(result);
        m_finished = true;
    }

    signal_completion();
}

}
