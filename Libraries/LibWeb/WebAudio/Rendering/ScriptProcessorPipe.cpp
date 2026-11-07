/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/WebAudio/Rendering/ScriptProcessorPipe.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio::Rendering {

struct ScriptProcessorPipeCallbacks : AtomicRefCounted<ScriptProcessorPipeCallbacks> {
    explicit ScriptProcessorPipeCallbacks(Core::EventLoop& loop)
        : event_loop(loop)
    {
    }
    Core::EventLoop& event_loop;
    Function<void(size_t, u64)> process_callback;
};

NonnullRefPtr<ScriptProcessorPipe> ScriptProcessorPipe::create(Config config, Core::EventLoop& event_loop)
{
    return adopt_ref(*new ScriptProcessorPipe(move(config), event_loop));
}

ScriptProcessorPipe::ScriptProcessorPipe(Config config, Core::EventLoop& event_loop)
    : m_config(move(config))
    , m_callbacks(adopt_ref(*new ScriptProcessorPipeCallbacks(event_loop)))
{
    RustFFI::PipeCallbacks callbacks {
        { m_callbacks.ptr(), [](void* service) { static_cast<ScriptProcessorPipeCallbacks*>(service)->ref(); }, [](void* service) { static_cast<ScriptProcessorPipeCallbacks*>(service)->unref(); } },
        [](void* service, void* task, void (*invoke)(void*)) {
            static_cast<ScriptProcessorPipeCallbacks*>(service)->event_loop.deferred_invoke([task, invoke] { invoke(task); });
        },
        [](void*) { },
        [](void* service, size_t block, u64 start_frame) {
            auto& callbacks = *static_cast<ScriptProcessorPipeCallbacks*>(service);
            if (!callbacks.process_callback)
                return false;
            callbacks.process_callback(block, start_frame);
            return true;
        },
    };
    RustFFI::ScriptProcessorPipeConfig native_config { m_config.buffer_size, m_config.input_channel_count, m_config.output_channel_count, m_config.block_count, m_config.waits_for_processing };
    m_pipe = RustFFI::web_audio_script_pipe_create(native_config, callbacks);
}

ScriptProcessorPipe::~ScriptProcessorPipe() { RustFFI::web_audio_script_pipe_destroy(m_pipe); }
Span<float> ScriptProcessorPipe::input_channel(size_t block, size_t channel)
{
    Span<float> samples;
    RustFFI::web_audio_script_pipe_channel(m_pipe, false, block, channel, &samples);
    return samples;
}
Span<float> ScriptProcessorPipe::output_channel(size_t block, size_t channel)
{
    Span<float> samples;
    RustFFI::web_audio_script_pipe_channel(m_pipe, true, block, channel, &samples);
    return samples;
}
bool ScriptProcessorPipe::has_output(size_t block) const { return RustFFI::web_audio_script_pipe_has_output(m_pipe, block); }
bool ScriptProcessorPipe::is_block_owned_by_rendering_thread(size_t block) const { return RustFFI::web_audio_script_pipe_is_rendering_owner(m_pipe, block); }
void ScriptProcessorPipe::hand_block_to_control_thread(size_t block, u64 frame) { RustFFI::web_audio_script_pipe_to_control(m_pipe, block, frame); }
void ScriptProcessorPipe::hand_block_back_to_rendering_thread(size_t block, bool has_output) { RustFFI::web_audio_script_pipe_to_rendering(m_pipe, block, has_output); }
void ScriptProcessorPipe::set_process_callback(Function<void(size_t, u64)> callback) { m_callbacks->process_callback = move(callback); }
void ScriptProcessorPipe::clear_process_callback() { m_callbacks->process_callback = nullptr; }
void ScriptProcessorPipe::shut_down() { RustFFI::web_audio_script_pipe_shutdown(m_pipe); }
bool ScriptProcessorPipe::is_shut_down() const { return RustFFI::web_audio_script_pipe_is_shutdown(m_pipe); }

}
