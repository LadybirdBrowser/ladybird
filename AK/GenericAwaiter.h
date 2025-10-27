/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Coroutine.h>
#include <AK/Error.h>
#include <AK/Function.h>

namespace AK {
class GenericAwaiter {
    AK_MAKE_NONMOVABLE(GenericAwaiter);
    AK_MAKE_NONCOPYABLE(GenericAwaiter);

public:
    GenericAwaiter(Function<void(Function<void()>)>&& fn)
    {
        fn([this] { ready(); });
    }

    bool await_ready() const { return false; }
    void await_suspend(std::coroutine_handle<> handle)
    {
        m_handle = handle;
    }
    ErrorOr<void> await_resume()
    {
        m_handle = {};
        return {};
    }

private:
    void ready()
    {
        if (m_handle)
            m_handle.resume();
    }

    std::coroutine_handle<> m_handle;
};
}

#ifdef USING_AK_GLOBALLY
using AK::GenericAwaiter;
#endif
