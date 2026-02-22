/*
 * Copyright (c) 2026, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <AK/Types.h>

namespace AK {

static inline void cpu_pause()
{
#if __has_builtin(__builtin_ia32_pause)
    __builtin_ia32_pause();
#elif __has_builtin(__builtin_arm_isb)
    __builtin_arm_isb(15);
#elif __has_builtin(__builtin_riscv_pause)
    __builtin_riscv_pause();
#endif
}

void yield_thread();

// As it is expected that this class may be constructed on the hot path each time, it has to be small
class Backoff final {
    AK_MAKE_NONCOPYABLE(Backoff);
    AK_MAKE_NONMOVABLE(Backoff);

public:
    Backoff() = default;
    ~Backoff() = default;

    ALWAYS_INLINE void tick()
    {
        if (m_step < 5) {
            // TODO: Implement a delay based on TPAUSE/MWAITX/WFET where possible.
            u32 iterations = 0;
#if ARCH(X86_64)
            // Anywhere between 50-230 cycles
            // Last step anywhere between 1600 - 7,300 cycles.
            iterations = 1u << m_step;
#elif ARCH(AARCH64)
            // Using ISB, ~40 cycles per instruction
            // Last step ~6400 cycles
            iterations = 4u << m_step;
#else
            // This needs tuning from someone that has these machines
            iterations = 1u << m_step;
#endif
            for (u64 i = 0; i < iterations; i++) {
                cpu_pause();
            }
            m_step += 1;
        } else {
            yield_thread();
        }
    }

private:
    u64 m_step = 0;
};

}

#ifdef USING_AK_GLOBALLY
using AK::Backoff;
using AK::cpu_pause;
using AK::yield_thread;
#endif
