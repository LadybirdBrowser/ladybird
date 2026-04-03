/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Profiler.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

// ASM interpreter register conventions (callee-saved, survive C++ calls):
//   x86_64:  r13 = pc
//   aarch64: x21 = pb+pc, x26 = pb  =>  pc = x21 - x26
static Optional<u32> read_program_counter_from_ucontext(ucontext_t const* uc)
{
#if defined(__x86_64__)
    auto r13 = static_cast<u64>(uc->uc_mcontext.gregs[REG_R13]);
    return static_cast<u32>(r13);
#elif defined(__aarch64__)
    auto x21 = static_cast<u64>(uc->uc_mcontext.regs[21]);
    auto x26 = static_cast<u64>(uc->uc_mcontext.regs[26]);
    return static_cast<u32>(x21 - x26);
#else
    (void)uc;
    return {};
#endif
}

namespace JS {

// The signal handler runs on the JS thread, which is effectively suspended
// while the handler executes — so the execution-context stack is stable and
// we can capture directly, same as the macOS Mach thread-suspend path.
static Atomic<Profiler*> s_active_profiler { nullptr };
static struct sigaction s_old_sigaction {};

static void signal_handler(int, siginfo_t*, void* ucontext)
{
    auto* profiler = s_active_profiler.load(AK::MemoryOrder::memory_order_relaxed);
    if (profiler) {
        auto pc = read_program_counter_from_ucontext(static_cast<ucontext_t const*>(ucontext));
        profiler->capture_sample(pc);
    }
}

u64 Profiler::os_tid() const
{
    return static_cast<u64>(m_js_thread);
}

bool Profiler::supports_timed_sampling() const
{
    return m_interval_us > 0;
}

bool Profiler::needs_bytecode_safe_points() const
{
    // Timed sampling captures from the signal handler directly (reads PC from
    // ucontext registers), so no safe points needed — ASM interpreter can run.
    // Without timed sampling (interval <= 0), tests use request_sample_for_test()
    // which requires safe-point polling in the CPP interpreter dispatch loop.
    return m_interval_us <= 0;
}

void Profiler::start()
{
    allocate_sample_buffer();
    if (m_interval_us <= 0)
        return;

    s_active_profiler.store(this, AK::MemoryOrder::memory_order_relaxed);
    m_sample_pending.store(false, AK::MemoryOrder::memory_order_relaxed);

    struct sigaction sa = {};
    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, &s_old_sigaction);
    m_platform_sampling_active = true;

    m_timer_running.store(true, AK::MemoryOrder::memory_order_relaxed);
    m_timer_thread = Threading::Thread::construct("JS Profiler"sv, [this]() -> intptr_t {
        while (m_timer_running.load(AK::MemoryOrder::memory_order_relaxed)) {
            usleep(m_interval_us);
            pthread_kill(m_js_thread, SIGUSR2);
        }
        return 0;
    });
    m_timer_thread->start();
}

void Profiler::stop()
{
    collect_and_free_samples();
    if (!m_platform_sampling_active)
        return;

    s_active_profiler.store(nullptr, AK::MemoryOrder::memory_order_relaxed);

    // Block SIGUSR2, drain any signal queued just before the timer thread exited,
    // then restore the previous handler.  Without the drain a pending SIGUSR2
    // delivered after restoration would use the old handler — potentially SIG_DFL (fatal).
    sigset_t only_usr2, old_mask;
    sigemptyset(&only_usr2);
    sigaddset(&only_usr2, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &only_usr2, &old_mask);
    struct timespec zero = { 0, 0 };
    sigtimedwait(&only_usr2, nullptr, &zero);
    sigaction(SIGUSR2, &s_old_sigaction, nullptr);
    pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
    m_platform_sampling_active = false;
}

}
