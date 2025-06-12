/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/StdLibExtras.h>

namespace AK {

template<typename T>
class Weakable;
template<typename T>
class WeakPtr;

class WeakLink : public RefCounted<WeakLink> {
    template<typename T>
    friend class Weakable;
    template<typename T>
    friend class WeakPtr;

public:
    template<typename T>
    RefPtr<T> strong_ref() const
    requires(IsBaseOf<RefCountedBase, T>)
    {
        return static_cast<T*>(m_ptr);
    }

    template<typename T>
    T* unsafe_ptr() const
    {
        return static_cast<T*>(m_ptr);
    }

    bool is_null() const { return m_ptr == nullptr; }

    void revoke() { m_ptr = nullptr; }

private:
    template<typename T>
    explicit WeakLink(T& weakable)
        : m_ptr(&weakable)
    {
    }

    mutable void* m_ptr { nullptr };
};

template<typename T>
class Weakable {
private:
    class Link;

public:
    template<typename U = T>
    WeakPtr<U> make_weak_ptr() const
    {
        return MUST(try_make_weak_ptr<U>());
    }

    template<typename U = T>
    ErrorOr<WeakPtr<U>> try_make_weak_ptr() const;

protected:
    Weakable() = default;

    ~Weakable()
    {
        revoke_weak_ptrs();
    }

    void revoke_weak_ptrs()
    {
        if (auto link = move(m_link))
            link->revoke();
    }

private:
    mutable RefPtr<WeakLink> m_link;
};

}

#if USING_AK_GLOBALLY
using AK::Weakable;
#endif
