/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Platform.h>
#include <AK/StackInfo.h>
#include <AK/StdLibExtras.h>
#include <stdio.h>
#include <string.h>

#ifdef AK_OS_SERENITY
#    include <serenity.h>
#elif defined(AK_OS_LINUX) || defined(AK_LIBC_GLIBC) || defined(AK_OS_MACOS) || defined(AK_OS_IOS) || defined(AK_OS_NETBSD) || defined(AK_OS_SOLARIS) || defined(AK_OS_HAIKU)
#    include <pthread.h>
#    include <sys/resource.h>
#    if defined(AK_OS_LINUX)
#        include <sys/auxv.h>
#    endif
#elif defined(AK_OS_FREEBSD) || defined(AK_OS_OPENBSD)
#    include <pthread.h>
#    include <pthread_np.h>
#elif defined(AK_OS_WINDOWS)
#    include <Windows.h>
// NOTE: Prevent clang-format from re-ordering this header order
#    include <Processthreadsapi.h>
#endif

namespace AK {

StackInfo::StackInfo()
{
    // Stack bounds never change; glibc queries them for the main thread by parsing /proc/self/maps.
    struct CachedBounds {
        FlatPtr base { 0 };
        size_t size { 0 };
    };
    static thread_local CachedBounds s_cached_bounds;
    if (s_cached_bounds.size != 0) {
        m_base = s_cached_bounds.base;
        m_size = s_cached_bounds.size;
        m_top = m_base + m_size;
        return;
    }

#ifdef AK_OS_SERENITY
    if (get_stack_bounds(&m_base, &m_size) < 0) {
        perror("get_stack_bounds");
        VERIFY_NOT_REACHED();
    }
#elif defined(AK_OS_LINUX) or defined(AK_LIBC_GLIBC) or defined(AK_OS_FREEBSD) or defined(AK_OS_NETBSD) or defined(AK_OS_SOLARIS) or defined(AK_OS_HAIKU)
    int rc;
    pthread_attr_t attr;
    pthread_attr_init(&attr);

#    if defined(AK_OS_LINUX) or defined(AK_LIBC_GLIBC) or defined(AK_OS_HAIKU)
    if ((rc = pthread_getattr_np(pthread_self(), &attr)) != 0) {
        fprintf(stderr, "pthread_getattr_np: %s\n", strerror(rc));
        VERIFY_NOT_REACHED();
    }
#    else
    if ((rc = pthread_attr_get_np(pthread_self(), &attr)) != 0) {
        fprintf(stderr, "pthread_attr_get_np: %s\n", strerror(rc));
        VERIFY_NOT_REACHED();
    }
#    endif

    if ((rc = pthread_attr_getstack(&attr, (void**)&m_base, &m_size)) != 0) {
        fprintf(stderr, "pthread_attr_getstack: %s\n", strerror(rc));
        VERIFY_NOT_REACHED();
    }
    pthread_attr_destroy(&attr);
#elif defined(AK_OS_MACOS) || defined(AK_OS_IOS)
    // NOTE: !! On MacOS, pthread_get_stackaddr_np gives the TOP of the stack, not the bottom!
    FlatPtr top_of_stack = (FlatPtr)pthread_get_stackaddr_np(pthread_self());
    m_size = (size_t)pthread_get_stacksize_np(pthread_self());
    // https://github.com/rust-lang/rust/issues/43347#issuecomment-316783599
    // https://developer.apple.com/library/archive/qa/qa1419/_index.html
    //
    // MacOS seems inconsistent on what stack size is given for the main thread.
    // According to the Apple docs, default for main thread is 8MB, and default for
    // other threads is 512KB
    if (pthread_main_np() == 1) {
        // Apparently the main thread's stack size is not reported correctly on macOS
        // but we can use getrlimit to get the correct value.
        rlimit limit {};
        getrlimit(RLIMIT_STACK, &limit);
        if (limit.rlim_cur == RLIM_INFINITY) {
            m_size = 8 * MiB;
        } else {
            m_size = limit.rlim_cur;
        }
    }
    m_base = top_of_stack - m_size;
#elif defined(AK_OS_OPENBSD)
    int rc;
    stack_t thread_stack;
    if ((rc = pthread_stackseg_np(pthread_self(), &thread_stack)) != 0) {
        fprintf(stderr, "pthread_stackseg_np: %s\n", strerror(rc));
        VERIFY_NOT_REACHED();
    }
    FlatPtr top_of_stack = (FlatPtr)thread_stack.ss_sp;
    m_size = (size_t)thread_stack.ss_size;
    m_base = top_of_stack - m_size;
#elif defined(AK_OS_WINDOWS)
    ULONG_PTR low_limit = 0;
    ULONG_PTR high_limit = 0;
    GetCurrentThreadStackLimits(&low_limit, &high_limit);

    // The stack overflows before reaching its low limit by the size reserved for handling that overflow.
    ULONG stack_guarantee = 0;
    SetThreadStackGuarantee(&stack_guarantee);
    low_limit += stack_guarantee;

    m_base = static_cast<FlatPtr>(low_limit);
    m_size = static_cast<size_t>(high_limit - low_limit);
#else
#    pragma message "StackInfo not supported on this platform! Recursion checks and stack scans may not work properly"
    m_size = (size_t)~0;
    m_base = 0;
#endif

    m_top = m_base + m_size;

#if defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID) && !defined(AK_LIBC_GLIBC)
    // musl reports only the part of the main thread's stack that is mapped so far, and from below the argument and
    // environment strings at its top. But the kernel grows the stack on demand until it spans the stack size limit, from
    // the top of its mapping. The kernel puts the executable's file name at that top, under one pointer, so the top
    // follows from where the file name ends.
    if (auto const* file_name = reinterpret_cast<char const*>(getauxval(AT_EXECFN)); file_name && getpid() == static_cast<pid_t>(gettid())) {
        auto page_size = static_cast<FlatPtr>(sysconf(_SC_PAGESIZE));
        auto file_name_end = reinterpret_cast<FlatPtr>(file_name) + strlen(file_name) + 1 + sizeof(void*);
        m_top = (file_name_end + page_size - 1) & ~(page_size - 1);
        rlimit limit;
        getrlimit(RLIMIT_STACK, &limit);
        rlim_t size = limit.rlim_cur;
        if (size == RLIM_INFINITY)
            size = 8 * 0x10000;
        // account for a guard page
        size -= static_cast<rlim_t>(page_size);
        // The stack reaches at least as far as musl reports, even where an unlimited size limit is taken as less.
        m_base = min(m_top - static_cast<FlatPtr>(size), m_base);
        m_size = m_top - m_base;
    }
#endif

    s_cached_bounds = { m_base, m_size };
}

}
