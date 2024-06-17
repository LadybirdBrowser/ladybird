/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Noncopyable.h>

#if defined(AK_OS_SERENITY)
#    include <mallocdefs.h>
#endif

namespace AK {

class NoAllocationGuard {
    AK_MAKE_NONCOPYABLE(NoAllocationGuard);
    AK_MAKE_NONMOVABLE(NoAllocationGuard);

public:
    NoAllocationGuard()
        : m_allocation_enabled_previously(get_thread_allocation_state())
    {
        set_thread_allocation_state(false);
    }

    ~NoAllocationGuard()
    {
        set_thread_allocation_state(m_allocation_enabled_previously);
    }

private:
    static bool get_thread_allocation_state()
    {
#if defined(AK_OS_SERENITY)
        // This extern thread-local lives in our LibC, which doesn't exist on other systems.
        return s_allocation_enabled;
#else
        return true;
#endif
    }

    static void set_thread_allocation_state(bool value)
    {
#if defined(AK_OS_SERENITY)
        s_allocation_enabled = value;
#else
        (void)value;
#endif
    }

    bool m_allocation_enabled_previously { true };
};

}

#if USING_AK_GLOBALLY
using AK::NoAllocationGuard;
#endif
