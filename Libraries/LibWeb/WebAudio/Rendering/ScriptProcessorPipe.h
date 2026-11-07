/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Types.h>
#include <LibCore/Forward.h>
#include <LibWeb/Export.h>

namespace Web::WebAudio::Rendering {

struct ScriptProcessorPipeCallbacks;

class WEB_API ScriptProcessorPipe final : public AtomicRefCounted<ScriptProcessorPipe> {
public:
    struct Config {
        size_t buffer_size { 0 };
        size_t input_channel_count { 0 };
        size_t output_channel_count { 0 };
        size_t block_count { 0 };
        bool waits_for_processing { false };
    };

    static NonnullRefPtr<ScriptProcessorPipe> create(Config, Core::EventLoop& control_thread_event_loop);

    ~ScriptProcessorPipe();
    void const* rust_pipe() const { return m_pipe; }

    void set_process_callback(Function<void(size_t block, u64 start_frame)>);
    void clear_process_callback();

private:
    ScriptProcessorPipe(Config, Core::EventLoop& control_thread_event_loop);

    NonnullRefPtr<ScriptProcessorPipeCallbacks> m_callbacks;
    void const* m_pipe { nullptr };
};

}
