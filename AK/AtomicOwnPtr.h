/*
 * Copyright (c) 2024, Ben Jilks <benjyjilks@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Forward.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>

namespace AK {

template<typename T, typename TDeleter>
class [[nodiscard]] AtomicOwnPtr {
public:
    AtomicOwnPtr() = default;

    AtomicOwnPtr(decltype(nullptr))
        : m_ptr(nullptr)
    {
    }

    AtomicOwnPtr(AtomicOwnPtr&& other)
        : m_ptr(other.leak_ptr())
    {
    }

    template<typename U>
    AtomicOwnPtr(NonnullOwnPtr<U>&& other)
        : m_ptr(other.leak_ptr())
    {
    }
    template<typename U>
    AtomicOwnPtr(AtomicOwnPtr<U>&& other)
        : m_ptr(other.leak_ptr())
    {
    }
    ~AtomicOwnPtr()
    {
        clear();
#ifdef SANITIZE_PTRS
        m_ptr = (T*)(explode_byte(OWNPTR_SCRUB_BYTE));
#endif
    }

    AtomicOwnPtr(AtomicOwnPtr const&) = delete;
    template<typename U>
    AtomicOwnPtr(AtomicOwnPtr<U> const&) = delete;
    AtomicOwnPtr& operator=(AtomicOwnPtr const&) = delete;
    template<typename U>
    AtomicOwnPtr& operator=(AtomicOwnPtr<U> const&) = delete;

    template<typename U>
    AtomicOwnPtr(NonnullOwnPtr<U> const&) = delete;
    template<typename U>
    AtomicOwnPtr& operator=(NonnullOwnPtr<U> const&) = delete;
    template<typename U>
    AtomicOwnPtr(RefPtr<U> const&) = delete;
    template<typename U>
    AtomicOwnPtr(NonnullRefPtr<U> const&) = delete;
    template<typename U>
    AtomicOwnPtr(WeakPtr<U> const&) = delete;
    template<typename U>
    AtomicOwnPtr& operator=(RefPtr<U> const&) = delete;
    template<typename U>
    AtomicOwnPtr& operator=(NonnullRefPtr<U> const&) = delete;
    template<typename U>
    AtomicOwnPtr& operator=(WeakPtr<U> const&) = delete;

    AtomicOwnPtr& operator=(AtomicOwnPtr&& other)
    {
        m_ptr.store(other.leak_ptr());
        return *this;
    }

    template<typename U>
    AtomicOwnPtr& operator=(AtomicOwnPtr<U>&& other)
    {
        m_ptr.store(other.leak_ptr());
        return *this;
    }

    template<typename U>
    AtomicOwnPtr& operator=(OwnPtr<U>&& other)
    {
        m_ptr.store(other.leak_ptr());
        return *this;
    }

    template<typename U>
    AtomicOwnPtr& operator=(NonnullOwnPtr<U>&& other)
    {
        m_ptr.store(other.leak_ptr());
        VERIFY(m_ptr.load());
        return *this;
    }

    AtomicOwnPtr& operator=(T* ptr) = delete;

    AtomicOwnPtr& operator=(nullptr_t)
    {
        clear();
        return *this;
    }

    void clear()
    {
        auto* ptr = m_ptr.load();
        m_ptr.store(nullptr);
        TDeleter {}(ptr);
    }

    bool operator!() const { return !m_ptr.load(); }

    [[nodiscard]] T* leak_ptr()
    {
        T* leaked_ptr = m_ptr.load();
        m_ptr.store(nullptr);
        return leaked_ptr;
    }

    NonnullOwnPtr<T> release_nonnull()
    {
        VERIFY(m_ptr);
        return NonnullOwnPtr<T>(NonnullOwnPtr<T>::Adopt, *leak_ptr());
    }

    template<typename U>
    NonnullOwnPtr<U> release_nonnull()
    {
        VERIFY(m_ptr);
        return NonnullOwnPtr<U>(NonnullOwnPtr<U>::Adopt, static_cast<U&>(*leak_ptr()));
    }

    T* ptr() const { return m_ptr.load(); }

    T* operator->() const
    {
        VERIFY(m_ptr);
        return m_ptr.load();
    }

    T& operator*() const
    {
        VERIFY(m_ptr);
        return *m_ptr.load();
    }

    operator T*() const { return m_ptr.load(); }

    operator bool() { return !!m_ptr.load(); }

    void swap(AtomicOwnPtr& other)
    {
        m_ptr.exchange(other.m_ptr);
    }

    template<typename U>
    void swap(AtomicOwnPtr<U>& other)
    {
        m_ptr.exchange(other.m_ptr);
    }

    static AtomicOwnPtr lift(T* ptr)
    {
        return AtomicOwnPtr { ptr };
    }

protected:
    explicit AtomicOwnPtr(T* ptr)
        : m_ptr(ptr)
    {
        static_assert(
            requires { requires typename T::AllowOwnPtr()(); } || !requires { requires !typename T::AllowOwnPtr()(); declval<T>().ref(); declval<T>().unref(); }, "Use RefPtr<> for RefCounted types");
    }

private:
    Atomic<T*> m_ptr { nullptr };
};

template<typename T, typename U>
inline void swap(AtomicOwnPtr<T>& a, AtomicOwnPtr<U>& b)
{
    a.swap(b);
}

template<typename T>
inline AtomicOwnPtr<T> adopt_atomic_own_if_nonnull(T* object)
{
    if (object)
        return AtomicOwnPtr<T>::lift(object);
    return {};
}

template<typename T>
struct Traits<AtomicOwnPtr<T>> : public DefaultTraits<AtomicOwnPtr<T>> {
    using PeekType = T*;
    using ConstPeekType = T const*;
    static unsigned hash(AtomicOwnPtr<T> const& p) { return ptr_hash(p.ptr()); }
    static bool equals(AtomicOwnPtr<T> const& a, AtomicOwnPtr<T> const& b) { return a.ptr() == b.ptr(); }
};

template<typename T>
struct Formatter<AtomicOwnPtr<T>> : Formatter<T*> {
    ErrorOr<void> format(FormatBuilder& builder, AtomicOwnPtr<T> const& value)
    {
        return Formatter<T*>::format(builder, value.ptr());
    }
};
}

#if USING_AK_GLOBALLY
using AK::adopt_atomic_own_if_nonnull;
using AK::AtomicOwnPtr;
#endif
