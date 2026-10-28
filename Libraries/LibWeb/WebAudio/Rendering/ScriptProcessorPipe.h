/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
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

    Config const& config() const { return m_config; }

    Span<float> input_channel(size_t block, size_t channel);
    Span<float> output_channel(size_t block, size_t channel);
    bool has_output(size_t block) const { return m_blocks[block].has_output; }

    bool is_block_owned_by_rendering_thread(size_t block) const;
    void hand_block_to_control_thread(size_t block, u64 start_frame);

    void set_process_callback(Function<void(size_t block, u64 start_frame)>);
    void clear_process_callback();
    void hand_block_back_to_rendering_thread(size_t block, bool has_output);

    void shut_down();
    bool is_shut_down() const { return m_is_shut_down.load(AK::MemoryOrder::memory_order_acquire); }

private:
    ScriptProcessorPipe(Config, Core::EventLoop& control_thread_event_loop);

    struct Block {
        Atomic<bool> is_owned_by_control_thread { false };
        bool has_output { false };
    };

    Config m_config;
    FixedArray<float> m_input_samples;
    FixedArray<float> m_output_samples;
    FixedArray<Block> m_blocks;

    Core::EventLoop& m_control_thread_event_loop;
    Function<void(size_t, u64)> m_process_callback;

    Mutex m_mutex;
    ConditionVariable m_block_returned { m_mutex };
    Atomic<bool> m_is_shut_down { false };
};

}
