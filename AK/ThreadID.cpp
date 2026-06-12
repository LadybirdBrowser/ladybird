/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ThreadID.h>

#if defined(AK_OS_WINDOWS)
#    include <windows.h>
#else
#    include <pthread.h>
#    include <unistd.h>
#endif
#if defined(AK_OS_FREEBSD)
#    include <pthread_np.h>
#endif

namespace AK {

static u64 query_current_thread_id()
{
#if defined(AK_OS_LINUX)
    return static_cast<u64>(gettid());
#elif defined(AK_OS_WINDOWS)
    return GetCurrentThreadId();
#elif defined(AK_OS_MACOS)
    u64 thread_id { 0 };
    pthread_threadid_np(nullptr, &thread_id);
    return thread_id;
#elif defined(AK_OS_FREEBSD)
    return static_cast<u64>(pthread_getthreadid_np());
#else
    return 0;
#endif
}

ThreadID ThreadID::current()
{
    static thread_local ThreadID const s_current_thread_id { query_current_thread_id() };
    return s_current_thread_id;
}

}
