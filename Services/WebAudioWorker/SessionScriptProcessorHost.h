/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/ScriptProcessor/ScriptProcessorHost.h>

namespace WebAudioWorker {

class WebAudioSession;

class SessionScriptProcessorHost final : public Web::WebAudio::Render::ScriptProcessorHost {
    // Due to our process model, ScriptProcessorNode ends up being a bit complex due to flow control
    // and having to shuttle buffers back and forth for script execution on the control thread in the
    // client process. This was an intentional tradeoff, as ScriptProcessorNode is deprecated and
    // we wanted to put Ladybird on a solid footing for a first class AudioWorkletNode implementation.
public:
    explicit SessionScriptProcessorHost(WebAudioSession&);

    virtual bool process_script_processor(
        Web::WebAudio::NodeID node_id,
        Web::WebAudio::Render::RenderContext& render_process_context,
        double playback_time_seconds,
        size_t buffer_size,
        size_t input_channel_count,
        size_t output_channel_count,
        Span<ReadonlySpan<f32>> input_channels,
        Span<Span<f32>> output_channels) override;

private:
    WebAudioSession& m_session;
};

}
