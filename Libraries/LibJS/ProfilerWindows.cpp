/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Profiler.h>

namespace JS {

// TODO: Implement the same sampler-thread + SuspendThread/GetThreadContext path
// used by V8 on Windows. The sampling path must stay minimal and non-blocking.
bool Profiler::supports_timed_sampling() const { return false; }
bool Profiler::needs_bytecode_safe_points() const { return true; }
void Profiler::start() { allocate_sample_buffer(); }
void Profiler::stop() { collect_and_free_samples(); }

u64 Profiler::os_tid() const
{
    // pthreads-win32 defines pthread_t as __ptw32_handle_t { void* p; unsigned x }.
    return reinterpret_cast<u64>(m_js_thread.p);
}

}
