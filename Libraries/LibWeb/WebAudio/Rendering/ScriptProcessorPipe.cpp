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
    : m_callbacks(adopt_ref(*new ScriptProcessorPipeCallbacks(event_loop)))
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
    RustFFI::ScriptProcessorPipeConfig native_config { config.buffer_size, config.input_channel_count, config.output_channel_count, config.block_count, config.waits_for_processing };
    m_pipe = RustFFI::web_audio_script_pipe_create(native_config, callbacks);
}

ScriptProcessorPipe::~ScriptProcessorPipe() { RustFFI::web_audio_script_pipe_destroy(m_pipe); }
void ScriptProcessorPipe::set_process_callback(Function<void(size_t, u64)> callback) { m_callbacks->process_callback = move(callback); }
void ScriptProcessorPipe::clear_process_callback() { m_callbacks->process_callback = nullptr; }

}
