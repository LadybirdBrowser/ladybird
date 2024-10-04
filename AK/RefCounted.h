/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Checked.h>
#include <AK/Noncopyable.h>
#include <AK/Platform.h>

namespace AK {

class RefCountedBase {
    AK_MAKE_NONCOPYABLE(RefCountedBase);
    AK_MAKE_NONMOVABLE(RefCountedBase);

public:
    using RefCountType = unsigned int;
    using AllowOwnPtr = FalseType;

    ALWAYS_INLINE void ref() const
    {
        VERIFY(m_ref_count > 0);
        VERIFY(!Checked<RefCountType>::addition_would_overflow(m_ref_count, 1));
        ++m_ref_count;
    }

    [[nodiscard]] bool try_ref() const
    {
        if (m_ref_count == 0)
            return false;
        ref();
        return true;
    }

    [[nodiscard]] RefCountType ref_count() const { return m_ref_count; }

protected:
    RefCountedBase() = default;
    ~RefCountedBase() { VERIFY(!m_ref_count); }

    ALWAYS_INLINE RefCountType deref_base() const
    {
        VERIFY(m_ref_count);
        return --m_ref_count;
    }

    RefCountType mutable m_ref_count { 1 };
};

template<typename T>
class RefCounted : public RefCountedBase {
public:
    bool unref() const
    {
        auto* that = const_cast<T*>(static_cast<T const*>(this));

        auto new_ref_count = deref_base();
        if (new_ref_count == 0) {
            if constexpr (requires { that->will_be_destroyed(); })
                that->will_be_destroyed();
            delete static_cast<T const*>(this);
            return true;
        }
        return false;
    }
};

}

#if USING_AK_GLOBALLY
using AK::RefCounted;
using AK::RefCountedBase;
#endif
