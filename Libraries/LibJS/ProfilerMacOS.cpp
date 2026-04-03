/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Profiler.h>
#include <mach/mach.h>
#include <unistd.h>

// ASM interpreter register conventions (callee-saved, survive C++ calls):
//   aarch64: x26 = bytecode base (pb), x21 = pb+pc  =>  pc = x21 - x26
//   x86_64:  r13 = pc
static Optional<u32> read_program_counter_from_suspended_thread(mach_port_t mach_thread)
{
#if defined(__aarch64__)
    arm_thread_state64_t state {};
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    bool ok = thread_get_state(mach_thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count) == KERN_SUCCESS;
    return ok ? Optional<u32>(static_cast<u32>(state.__x[21] - state.__x[26])) : Optional<u32> {};
#elif defined(__x86_64__)
    x86_thread_state64_t state {};
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    bool ok = thread_get_state(mach_thread, x86_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count) == KERN_SUCCESS;
    return ok ? Optional<u32>(static_cast<u32>(state.__r13)) : Optional<u32> {};
#else
    return {};
#endif
}

namespace JS {

bool Profiler::supports_timed_sampling() const { return m_interval_us > 0; }
bool Profiler::needs_bytecode_safe_points() const { return m_interval_us <= 0; }

void Profiler::start()
{
    allocate_sample_buffer();
    if (m_interval_us <= 0)
        return;

    auto mach_thread = pthread_mach_thread_np(m_js_thread);
    m_platform_sampling_active = true;

    m_timer_running.store(true, AK::MemoryOrder::memory_order_relaxed);
    m_timer_thread = Threading::Thread::construct("JS Profiler"sv, [this, mach_thread]() -> intptr_t {
        while (m_timer_running.load(AK::MemoryOrder::memory_order_relaxed)) {
            usleep(m_interval_us);
            if (thread_suspend(mach_thread) != KERN_SUCCESS)
                continue;
            if (auto pc = read_program_counter_from_suspended_thread(mach_thread); pc.has_value())
                capture_sample(pc);
            thread_resume(mach_thread);
        }
        return 0;
    });
    m_timer_thread->start();
}

void Profiler::stop()
{
    collect_and_free_samples();
    m_platform_sampling_active = false;
}

u64 Profiler::os_tid() const
{
    return reinterpret_cast<u64>(m_js_thread);
}

}
