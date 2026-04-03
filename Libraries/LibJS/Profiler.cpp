/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Profiler.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/SourceRange.h>

namespace JS {

static i64 current_epoch_ms()
{
    return UnixDateTime::now().milliseconds_since_epoch();
}

Profiler::Profiler(VM& vm, int interval_us)
    : m_vm(vm)
    , m_interval_us(interval_us)
{
}

Profiler::~Profiler()
{
    stop();
}

void Profiler::sample_if_needed()
{
    if (!m_sample_pending.exchange(false, AK::MemoryOrder::memory_order_relaxed))
        return;
    capture_sample({});
}

void Profiler::request_sample_for_test()
{
    m_sample_pending.store(true, AK::MemoryOrder::memory_order_relaxed);
}

void Profiler::reset_state_for_start()
{
    m_start_time = MonotonicTime::now();
    m_start_epoch_ms = current_epoch_ms();
    m_stop_epoch_ms = 0;
    m_js_thread = pthread_self();
    m_raw_sample_count.store(0, AK::MemoryOrder::memory_order_relaxed);
    m_sample_pending.store(false, AK::MemoryOrder::memory_order_relaxed);
    m_raw_samples = new RawSample[MAX_RAW_SAMPLES];
    m_string_table.clear();
    m_string_map.clear();
    m_frame_table.clear();
    m_frame_map.clear();
    m_stack_table.clear();
    m_stack_map.clear();
    m_samples.clear();
}

void Profiler::reserve_output_tables()
{
    m_string_table.ensure_capacity(256);
    m_frame_table.ensure_capacity(256);
    m_stack_table.ensure_capacity(1024);
}

void Profiler::allocate_raw_samples()
{
    delete[] m_raw_samples;
    m_raw_samples = new RawSample[MAX_RAW_SAMPLES];
}

void Profiler::process_and_free_raw_samples()
{
    if (!m_raw_samples)
        return;

    process_raw_samples();
    delete[] m_raw_samples;
    m_raw_samples = nullptr;
}

void Profiler::stop_timer_thread()
{
    m_timer_running.store(false, AK::MemoryOrder::memory_order_relaxed);
    if (!m_timer_thread)
        return;

    (void)m_timer_thread->join();
    m_timer_thread = nullptr;
}

double Profiler::elapsed_ms_since_start() const
{
    auto elapsed = MonotonicTime::now() - m_start_time;
    return static_cast<double>(max(elapsed.to_microseconds(), static_cast<i64>(0))) / 1000.0;
}

void Profiler::allocate_sample_buffer()
{
    reset_state_for_start();
    allocate_raw_samples();
    reserve_output_tables();
}

void Profiler::collect_and_free_samples()
{
    m_stop_epoch_ms = current_epoch_ms();
    stop_timer_thread();
    process_and_free_raw_samples();
}

void Profiler::capture_frames(RawSample& tick, Optional<u32> leaf_program_counter)
{
    auto const& ec_stack = m_vm.execution_context_stack();
    u32 frame_count = 0;

    for (ssize_t i = static_cast<ssize_t>(ec_stack.size()) - 1; i >= 0 && frame_count < MAX_STACK_DEPTH; --i) {
        auto* ctx = ec_stack[i];
        if (!ctx || !ctx->executable)
            continue;

        // Use the register-derived offset only for the topmost frame (macOS Mach path).
        // On the Linux safe-point path leaf_program_counter is absent and ctx->program_counter
        // is used for all frames. For inner frames on macOS (frame_count > 0),
        // ctx->program_counter may be stale when the ASM interpreter is running.
        auto program_counter = (frame_count == 0 && leaf_program_counter.has_value())
            ? *leaf_program_counter
            : ctx->program_counter;

        auto& frame = tick.frames[frame_count++];
        frame.executable = bit_cast<FlatPtr>(ctx->executable.ptr());
        frame.program_counter = program_counter;
        frame.name = ctx->executable->name;
    }

    tick.frame_count = frame_count;
}

void Profiler::capture_sample(Optional<u32> leaf_program_counter)
{
    auto index = m_raw_sample_count.load(AK::MemoryOrder::memory_order_relaxed);
    if (index >= MAX_RAW_SAMPLES || !m_raw_samples)
        return;

    auto& tick = m_raw_samples[index];
    tick.time_ms = elapsed_ms_since_start();
    capture_frames(tick, leaf_program_counter);
    m_raw_sample_count.store(index + 1, AK::MemoryOrder::memory_order_relaxed);
}

void Profiler::process_raw_samples()
{
    auto count = min(m_raw_sample_count.load(AK::MemoryOrder::memory_order_relaxed), MAX_RAW_SAMPLES);
    m_samples.ensure_capacity(count);

    for (u32 i = 0; i < count; ++i) {
        auto const& tick = m_raw_samples[i];
        if (tick.frame_count == 0 || tick.frame_count > MAX_STACK_DEPTH)
            continue;
        m_samples.append({ tick.time_ms, intern_stack_trace(tick) });
    }
}

u32 Profiler::intern_stack_trace(RawSample const& tick)
{
    Optional<u32> prefix;

    for (ssize_t i = static_cast<ssize_t>(tick.frame_count) - 1; i >= 0; --i) {
        auto const& frame = tick.frames[i];
        auto const* executable = bit_cast<Bytecode::Executable const*>(frame.executable);

        u32 line = 0;
        u32 column = 0;
        String filename;

        if (executable && frame.program_counter < executable->bytecode.size()) {
            auto unrealized = executable->source_range_at(frame.program_counter);
            if (unrealized.source_code) {
                auto range = unrealized.realize();
                line = range.start.line;
                column = range.start.column;
                filename = MUST(String::from_byte_string(range.filename()));
            }
        }

        String function_name = frame.name.is_empty()
            ? "(anonymous)"_string
            : MUST(String::formatted("{}", frame.name));

        String location = filename.is_empty()
            ? function_name
            : MUST(String::formatted("{} ({}:{}:{})", function_name, filename, line, column));

        auto frame_index = intern_frame(location, line, column);
        u64 key = (static_cast<u64>(frame_index) << 32) | prefix.value_or(UINT32_MAX);

        if (auto it = m_stack_map.find(key); it != m_stack_map.end()) {
            prefix = it->value;
        } else {
            u32 stack_index = m_stack_table.size();
            m_stack_table.append({ frame_index, prefix });
            m_stack_map.set(key, stack_index);
            prefix = stack_index;
        }
    }

    return prefix.value_or(0);
}

u32 Profiler::intern_string(String const& str)
{
    if (auto it = m_string_map.find(str); it != m_string_map.end())
        return it->value;
    u32 index = m_string_table.size();
    m_string_table.append(str);
    m_string_map.set(str, index);
    return index;
}

u32 Profiler::intern_frame(String const& location, u32 line, u32 column)
{
    // The location string encodes function name + source position, so string_index
    // is a unique key for each distinct (function, file, line, col) combination.
    auto string_index = intern_string(location);
    if (auto it = m_frame_map.find(string_index); it != m_frame_map.end())
        return it->value;
    u32 index = m_frame_table.size();
    m_frame_table.append({ string_index, line, column });
    m_frame_map.set(string_index, index);
    return index;
}

}
