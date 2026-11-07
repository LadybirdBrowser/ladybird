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
#include <AK/FixedArray.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Span.h>
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

    Config const& config() const { return m_config; }

    Span<float> input_channel(size_t block, size_t channel);
    Span<float> output_channel(size_t block, size_t channel);
    bool has_output(size_t block) const;

    bool is_block_owned_by_rendering_thread(size_t block) const;
    void hand_block_to_control_thread(size_t block, u64 start_frame);

    void set_process_callback(Function<void(size_t block, u64 start_frame)>);
    void clear_process_callback();
    void hand_block_back_to_rendering_thread(size_t block, bool has_output);

    void shut_down();
    bool is_shut_down() const;

private:
    ScriptProcessorPipe(Config, Core::EventLoop& control_thread_event_loop);

    Config m_config;
    NonnullRefPtr<ScriptProcessorPipeCallbacks> m_callbacks;
    void const* m_pipe { nullptr };
};

}
