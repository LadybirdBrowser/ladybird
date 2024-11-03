/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Atomic.h>
#include <AK/Checked.h>
#include <AK/Noncopyable.h>
#include <AK/Platform.h>

namespace AK {

class AtomicRefCountedBase {
    AK_MAKE_NONCOPYABLE(AtomicRefCountedBase);
    AK_MAKE_NONMOVABLE(AtomicRefCountedBase);

public:
    using RefCountType = unsigned int;
    using AllowOwnPtr = FalseType;

    void ref() const
    {
        auto old_ref_count = m_ref_count.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
        VERIFY(old_ref_count > 0);
        VERIFY(!Checked<RefCountType>::addition_would_overflow(old_ref_count, 1));
    }

    [[nodiscard]] bool try_ref() const
    {
        RefCountType expected = m_ref_count.load(AK::MemoryOrder::memory_order_relaxed);
        for (;;) {
            if (expected == 0)
                return false;
            VERIFY(!Checked<RefCountType>::addition_would_overflow(expected, 1));
            if (m_ref_count.compare_exchange_strong(expected, expected + 1, AK::MemoryOrder::memory_order_acquire))
                return true;
        }
    }

    [[nodiscard]] RefCountType ref_count() const
    {
        return m_ref_count.load(AK::MemoryOrder::memory_order_relaxed);
    }

protected:
    AtomicRefCountedBase() = default;
    ~AtomicRefCountedBase()
    {
        VERIFY(m_ref_count.load(AK::MemoryOrder::memory_order_relaxed) == 0);
    }

    RefCountType deref_base() const
    {
        auto old_ref_count = m_ref_count.fetch_sub(1, AK::MemoryOrder::memory_order_acq_rel);
        VERIFY(old_ref_count > 0);
        return old_ref_count - 1;
    }

    mutable Atomic<RefCountType> m_ref_count { 1 };
};

template<typename T>
class AtomicRefCounted : public AtomicRefCountedBase {
public:
    bool unref() const
    {
        auto* that = const_cast<T*>(static_cast<T const*>(this));
        auto new_ref_count = deref_base();
        if (new_ref_count == 0) {
            if constexpr (requires { that->will_be_destroyed(); })
                that->will_be_destroyed();
            delete that;
            return true;
        }
        return false;
    }
};

}

#if USING_AK_GLOBALLY
using AK::AtomicRefCounted;
using AK::AtomicRefCountedBase;
#endif
