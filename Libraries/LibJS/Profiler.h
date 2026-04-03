/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibThreading/Thread.h>
#include <pthread.h>

namespace JS {

class JS_API Profiler {
public:
    explicit Profiler(VM&, int interval_us = 1000);
    ~Profiler();

    void start();
    void stop();

    // Captures a pending sample at the next bytecode dispatch boundary. Linux uses
    // this for signal-driven safe-point sampling; tests also use it explicitly.
    void sample_if_needed();
    void request_sample_for_test();
    bool supports_timed_sampling() const;
    bool needs_bytecode_safe_points() const;
    void capture_sample(Optional<u32> leaf_program_counter);

    struct Frame {
        u32 string_index;
        u32 line;
        u32 column;
    };
    struct Stack {
        u32 frame_index;
        Optional<u32> prefix;
    };
    struct Sample {
        double time_ms;
        u32 stack_index;
    };

    Vector<String> const& string_table() const { return m_string_table; }
    Vector<Frame> const& frame_table() const { return m_frame_table; }
    Vector<Stack> const& stack_table() const { return m_stack_table; }
    Vector<Sample> const& samples() const { return m_samples; }
    int interval_us() const { return m_interval_us; }
    i64 start_time_epoch_ms() const;
    i64 stop_time_epoch_ms() const;
    u64 os_tid() const;

private:
    static constexpr u32 MAX_STACK_DEPTH = 64;
    static constexpr u32 MAX_RAW_SAMPLES = 16384;

    // capture_sample() runs either in a signal handler (Linux) or while the JS thread
    // is suspended via Mach (macOS).  In both cases the GC cannot run, so GC-managed
    // objects on the execution-context stack are alive — but only for the duration of
    // capture_sample() itself.  Once the JS thread resumes the GC is free to collect them,
    // so by the time process_raw_samples() runs they may be gone.
    //
    // Consequences for UnprocessedFrame:
    //  - No GC::Ptr: raw GC pointers are only safe to read during capture_sample().
    //  - No heap allocation: malloc may deadlock if the suspended thread was inside it.
    //  - The frame name is copied from Bytecode::Executable::name while the executable is live.
    struct UnprocessedFrame {
        FlatPtr executable;  // Bytecode::Executable const* — GC cell, valid only during capture_sample()
        u32 program_counter; // offset into executable->bytecode (not a machine PC)
        Utf16FlyString name;
    };
    struct RawSample {
        double time_ms;
        u32 frame_count;
        UnprocessedFrame frames[MAX_STACK_DEPTH];
    };

    // Async-signal-safe: called from a POSIX signal handler (Linux) or while the JS
    // thread is Mach-suspended (macOS).  Must not allocate or call non-reentrant
    // functions — see UnprocessedFrame comment above.
    // leaf_program_counter: register-derived PC for the topmost frame (macOS path);
    // absent on the Linux safe-point path, where ctx->program_counter is used instead.
    void reset_state_for_start();
    void reserve_output_tables();
    void allocate_raw_samples();
    void process_and_free_raw_samples();
    void capture_frames(RawSample&, Optional<u32> leaf_program_counter);
    void stop_timer_thread();
    double elapsed_ms_since_start() const;
    void allocate_sample_buffer();
    void collect_and_free_samples();

    void process_raw_samples();
    u32 intern_string(String const&);
    u32 intern_frame(String const& location, u32 line, u32 column);
    u32 intern_stack_trace(RawSample const&);

    VM& m_vm;
    int m_interval_us;
    ::MonotonicTime m_start_time { ::MonotonicTime::now() };
    i64 m_start_epoch_ms { 0 };
    i64 m_stop_epoch_ms { 0 };
    Atomic<bool> m_timer_running { false };
    RefPtr<Threading::Thread> m_timer_thread;
    pthread_t m_js_thread {};

    RawSample* m_raw_samples { nullptr };
    Atomic<u32> m_raw_sample_count { 0 };

    // Set either by the Linux signal handler or by tests requesting a sample,
    // then consumed by sample_if_needed() at the next safe bytecode boundary.
    Atomic<bool> m_sample_pending { false };
    bool m_platform_sampling_active { false };

    Vector<String> m_string_table;
    HashMap<String, u32> m_string_map;
    Vector<Frame> m_frame_table;
    // Key: string_index (location string uniquely encodes function + source position)
    HashMap<u32, u32> m_frame_map;
    Vector<Stack> m_stack_table;
    HashMap<u64, u32> m_stack_map;
    Vector<Sample> m_samples;
};

inline i64 Profiler::start_time_epoch_ms() const
{
    return m_start_epoch_ms;
}

inline i64 Profiler::stop_time_epoch_ms() const
{
    return m_stop_epoch_ms;
}

}
