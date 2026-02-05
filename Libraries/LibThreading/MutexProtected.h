/*
 * Copyright (c) 2022, kleines Filmröllchen <malu.bertsch@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Noncopyable.h>
#include <LibThreading/Mutex.h>

namespace Threading {

template<typename T>
class MutexProtected {
    AK_MAKE_NONCOPYABLE(MutexProtected);
    AK_MAKE_NONMOVABLE(MutexProtected);

public:
    using ProtectedType = T;

    ALWAYS_INLINE MutexProtected() = default;
    ALWAYS_INLINE MutexProtected(T&& value)
        : m_value(move(value))
    {
    }
    ALWAYS_INLINE explicit MutexProtected(T& value)
        : m_value(value)
    {
    }

    template<typename Callback>
    decltype(auto) with_locked(Callback callback)
    {
        auto lock = this->lock();
        // This allows users to get a copy, but if we don't allow references through &m_value, it's even more complex.
        return callback(m_value);
    }

    template<VoidFunction<T> Callback>
    void for_each_locked(Callback callback)
    {
        with_locked([&](auto& value) {
            for (auto& item : value)
                callback(item);
        });
    }

private:
    [[nodiscard]] ALWAYS_INLINE MutexLocker lock() { return MutexLocker(m_lock); }

    T m_value;
    Mutex m_lock {};
};

}
