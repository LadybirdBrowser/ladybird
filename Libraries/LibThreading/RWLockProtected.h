/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Noncopyable.h>
#include <LibThreading/RWLock.h>

namespace Threading {

template<typename T>
class RWLockProtected {
    AK_MAKE_NONCOPYABLE(RWLockProtected);
    AK_MAKE_NONMOVABLE(RWLockProtected);

public:
    using ProtectedType = T;

    ALWAYS_INLINE RWLockProtected() = default;
    ALWAYS_INLINE RWLockProtected(T&& value)
        : m_value(move(value))
    {
    }
    ALWAYS_INLINE explicit RWLockProtected(T& value)
        : m_value(value)
    {
    }

    template<typename Callback>
    requires(requires { declval<Callback>()(declval<T const&>()); })
    decltype(auto) with_read_locked(Callback callback) const
    {
        auto lock = this->lock_read();
        return callback(m_value);
    }

    template<typename Callback>
    decltype(auto) with_write_locked(Callback callback)
    {
        auto lock = this->lock_write();
        return callback(m_value);
    }

    template<VoidFunction<T> Callback>
    void for_each_locked(Callback callback)
    {
        with_read_locked([&](auto const& value) {
            for (auto& item : value)
                callback(item);
        });
    }

private:
    [[nodiscard]] ALWAYS_INLINE RWLockLocker<LockMode::Read> lock_read() const
    {
        return RWLockLocker<LockMode::Read> { m_lock };
    }

    [[nodiscard]] ALWAYS_INLINE RWLockLocker<LockMode::Write> lock_write()
    {
        return RWLockLocker<LockMode::Write> { m_lock };
    }

    T m_value;
    mutable RWLock m_lock {};
};

}
