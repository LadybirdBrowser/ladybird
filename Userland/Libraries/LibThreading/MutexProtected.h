/*
 * Copyright (c) 2022, kleines Filmröllchen <malu.bertsch@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <LibThreading/Mutex.h>

namespace Threading {

template<typename T>
class MutexProtected {
    AK_MAKE_NONCOPYABLE(MutexProtected);
    AK_MAKE_NONMOVABLE(MutexProtected);

    template<typename Callback>
    using CallbackReturnType = decltype(declval<Callback>()(declval<T&>()));

    template<typename Callback>
    using TryLockReturnType = Conditional<IsVoid<CallbackReturnType<Callback>>, void, Optional<CallbackReturnType<Callback>>>;

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

    template<typename Callback>
    auto try_with_locked(Callback callback) -> TryLockReturnType<Callback>
    {
        if (auto lock = this->try_lock(); lock.has_value()) {
            return callback(m_value);
        }

        return TryLockReturnType<Callback>();
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
    [[nodiscard]] ALWAYS_INLINE Optional<MutexLocker> try_lock() { return MutexLocker::try_lock(m_lock); }

    T m_value;
    Mutex m_lock {};
};

}
