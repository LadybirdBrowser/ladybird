/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/WebAudio/Rendering/ScriptProcessorPipe.h>

namespace Web::WebAudio::Rendering {

// Moves a ScriptProcessorNode's blocks of buffer_size frames between the rendering thread and the control thread. The
// render node cycles through block_count blocks, so a block's output plays block_count blocks after its input was
// captured. A block the control thread has not handed back in time renders silence and drops its input, unless the
// rendering thread waits for processing.
NonnullRefPtr<ScriptProcessorPipe> ScriptProcessorPipe::create(Config config, Core::EventLoop& control_thread_event_loop)
{
    VERIFY(config.buffer_size > 0);
    VERIFY(config.block_count >= 2);
    return adopt_ref(*new ScriptProcessorPipe(config, control_thread_event_loop));
}

ScriptProcessorPipe::ScriptProcessorPipe(Config config, Core::EventLoop& control_thread_event_loop)
    : m_config(config)
    , m_input_samples(MUST(FixedArray<float>::create(config.block_count * config.input_channel_count * config.buffer_size)))
    , m_output_samples(MUST(FixedArray<float>::create(config.block_count * config.output_channel_count * config.buffer_size)))
    , m_blocks(MUST(FixedArray<Block>::create(config.block_count)))
    , m_control_thread_event_loop(control_thread_event_loop)
{
}

Span<float> ScriptProcessorPipe::input_channel(size_t block, size_t channel)
{
    VERIFY(block < m_config.block_count);
    VERIFY(channel < m_config.input_channel_count);
    return m_input_samples.span().slice((block * m_config.input_channel_count + channel) * m_config.buffer_size, m_config.buffer_size);
}

Span<float> ScriptProcessorPipe::output_channel(size_t block, size_t channel)
{
    VERIFY(block < m_config.block_count);
    VERIFY(channel < m_config.output_channel_count);
    return m_output_samples.span().slice((block * m_config.output_channel_count + channel) * m_config.buffer_size, m_config.buffer_size);
}

bool ScriptProcessorPipe::is_block_owned_by_rendering_thread(size_t block) const
{
    return !m_blocks[block].is_owned_by_control_thread.load(AK::MemoryOrder::memory_order_acquire);
}

void ScriptProcessorPipe::hand_block_to_control_thread(size_t block, u64 start_frame)
{
    m_blocks[block].is_owned_by_control_thread.store(true, AK::MemoryOrder::memory_order_release);
    m_control_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), block, start_frame] {
        if (self->m_process_callback && !self->is_shut_down())
            self->m_process_callback(block, start_frame);
        else
            self->hand_block_back_to_rendering_thread(block, false);
    });

    if (!m_config.waits_for_processing)
        return;
    MutexLocker locker(m_mutex);
    m_block_returned.wait_while([&] {
        return !is_block_owned_by_rendering_thread(block) && !is_shut_down();
    });
}

void ScriptProcessorPipe::set_process_callback(Function<void(size_t, u64)> process_callback)
{
    m_process_callback = move(process_callback);
}

void ScriptProcessorPipe::clear_process_callback()
{
    m_process_callback = nullptr;
}

void ScriptProcessorPipe::hand_block_back_to_rendering_thread(size_t block, bool has_output)
{
    m_blocks[block].has_output = has_output;
    m_blocks[block].is_owned_by_control_thread.store(false, AK::MemoryOrder::memory_order_release);

    // The lock orders this notification after a waiting rendering thread has checked the block's owner.
    MutexLocker locker(m_mutex);
    m_block_returned.broadcast();
}

void ScriptProcessorPipe::shut_down()
{
    m_is_shut_down.store(true, AK::MemoryOrder::memory_order_release);
    MutexLocker locker(m_mutex);
    m_block_returned.broadcast();
}

}
