/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/StdLibExtras.h>

namespace AK {

template<typename Callback>
class [[nodiscard]] ScopeGuard {
    AK_MAKE_NONCOPYABLE(ScopeGuard);
    AK_MAKE_NONMOVABLE(ScopeGuard);

public:
    ScopeGuard(Callback callback)
        : m_callback(move(callback))
    {
    }

    ~ScopeGuard()
    {
        m_callback();
    }

private:
    Callback m_callback;
};

template<typename Callback>
class [[nodiscard]] ArmedScopeGuard {
    AK_MAKE_NONCOPYABLE(ArmedScopeGuard);
    AK_MAKE_NONMOVABLE(ArmedScopeGuard);

public:
    ArmedScopeGuard(Callback callback)
        : m_callback(move(callback))
    {
    }

    ~ArmedScopeGuard()
    {
        if (m_armed)
            m_callback();
    }

    void disarm() { m_armed = false; }

private:
    Callback m_callback;
    bool m_armed { true };
};

}

#if USING_AK_GLOBALLY
using AK::ArmedScopeGuard;
using AK::ScopeGuard;
#endif
