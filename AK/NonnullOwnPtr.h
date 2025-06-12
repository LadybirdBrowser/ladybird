/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Format.h>
#include <AK/RefCounted.h>
#include <AK/StdLibExtras.h>
#include <AK/Traits.h>
#include <AK/Types.h>

#define NONNULLOWNPTR_SCRUB_BYTE 0xf1

namespace AK {

template<typename T>
class WeakPtr;

template<typename T>
class [[nodiscard]] NonnullOwnPtr {
public:
    using ElementType = T;

    enum AdoptTag { Adopt };

    NonnullOwnPtr(AdoptTag, T& ptr)
        : m_ptr(&ptr)
    {
        static_assert(
            requires { requires typename T::AllowOwnPtr()(); } || !requires { requires !typename T::AllowOwnPtr()(); declval<T>().ref(); declval<T>().unref(); },
            "Use NonnullRefPtr<> for RefCounted types");
    }
    NonnullOwnPtr(NonnullOwnPtr&& other)
        : m_ptr(other.leak_ptr())
    {
        VERIFY(m_ptr);
    }
    template<typename U>
    NonnullOwnPtr(NonnullOwnPtr<U>&& other)
        : m_ptr(other.leak_ptr())
    {
        VERIFY(m_ptr);
    }
    ~NonnullOwnPtr()
    {
        clear();
#ifdef SANITIZE_PTRS
        m_ptr = (T*)(explode_byte(NONNULLOWNPTR_SCRUB_BYTE));
#endif
    }

    NonnullOwnPtr(NonnullOwnPtr const&) = delete;
    template<typename U>
    NonnullOwnPtr(NonnullOwnPtr<U> const&) = delete;
    NonnullOwnPtr& operator=(NonnullOwnPtr const&) = delete;
    template<typename U>
    NonnullOwnPtr& operator=(NonnullOwnPtr<U> const&) = delete;

    template<typename U>
    NonnullOwnPtr(RefPtr<U> const&) = delete;
    template<typename U>
    NonnullOwnPtr(NonnullRefPtr<U> const&) = delete;
    template<typename U>
    NonnullOwnPtr(WeakPtr<U> const&) = delete;
    template<typename U>
    NonnullOwnPtr& operator=(RefPtr<U> const&) = delete;
    template<typename U>
    NonnullOwnPtr& operator=(NonnullRefPtr<U> const&) = delete;
    template<typename U>
    NonnullOwnPtr& operator=(WeakPtr<U> const&) = delete;

    NonnullOwnPtr& operator=(NonnullOwnPtr&& other)
    {
        NonnullOwnPtr ptr(move(other));
        swap(ptr);
        return *this;
    }

    template<typename U>
    NonnullOwnPtr& operator=(NonnullOwnPtr<U>&& other)
    {
        NonnullOwnPtr ptr(move(other));
        swap(ptr);
        return *this;
    }

    [[nodiscard]] T* leak_ptr()
    {
        return exchange(m_ptr, nullptr);
    }

    ALWAYS_INLINE RETURNS_NONNULL T* ptr() const
    {
        VERIFY(m_ptr);
        return m_ptr;
    }

    ALWAYS_INLINE RETURNS_NONNULL T* operator->() const { return ptr(); }

    [[nodiscard]] ALWAYS_INLINE T& operator*() const { return *ptr(); }

    ALWAYS_INLINE RETURNS_NONNULL operator T*() const { return ptr(); }

    operator bool() const = delete;
    bool operator!() const = delete;

    void swap(NonnullOwnPtr& other)
    {
        AK::swap(m_ptr, other.m_ptr);
    }

    template<typename U>
    void swap(NonnullOwnPtr<U>& other)
    {
        AK::swap(m_ptr, other.m_ptr);
    }

    template<typename U>
    NonnullOwnPtr<U> release_nonnull()
    {
        VERIFY(m_ptr);
        return NonnullOwnPtr<U>(NonnullOwnPtr<U>::Adopt, static_cast<U&>(*leak_ptr()));
    }

private:
    void clear()
    {
        auto* ptr = exchange(m_ptr, nullptr);
        delete ptr;
    }

    T* m_ptr = nullptr;
};

template<typename T>
inline NonnullOwnPtr<T> adopt_own(T& object)
{
    return NonnullOwnPtr<T>(NonnullOwnPtr<T>::Adopt, object);
}

template<class T, class... Args>
requires(IsConstructible<T, Args...>) inline NonnullOwnPtr<T> make(Args&&... args)
{
    return NonnullOwnPtr<T>(NonnullOwnPtr<T>::Adopt, *new T(forward<Args>(args)...));
}

// Use like `adopt_nonnull_own_or_enomem(new (nothrow) T(args...))`.
template<typename T>
inline ErrorOr<NonnullOwnPtr<T>> adopt_nonnull_own_or_enomem(T* object)
{
    if (!object)
        return Error::from_errno(ENOMEM);
    return NonnullOwnPtr<T>(NonnullOwnPtr<T>::Adopt, *object);
}

template<typename T, class... Args>
requires(IsConstructible<T, Args...>) inline ErrorOr<NonnullOwnPtr<T>> try_make(Args&&... args)
{
    return adopt_nonnull_own_or_enomem(new (nothrow) T(forward<Args>(args)...));
}

template<typename T>
struct Traits<NonnullOwnPtr<T>> : public DefaultTraits<NonnullOwnPtr<T>> {
    using PeekType = T*;
    using ConstPeekType = T const*;
    static unsigned hash(NonnullOwnPtr<T> const& p) { return ptr_hash(p.ptr()); }
    static bool equals(NonnullOwnPtr<T> const& a, NonnullOwnPtr<T> const& b) { return a.ptr() == b.ptr(); }
};

template<typename T, typename U>
inline void swap(NonnullOwnPtr<T>& a, NonnullOwnPtr<U>& b)
{
    a.swap(b);
}

template<Formattable T>
struct Formatter<NonnullOwnPtr<T>> : Formatter<T> {
    ErrorOr<void> format(FormatBuilder& builder, NonnullOwnPtr<T> const& value)
    {
        return Formatter<T>::format(builder, *value);
    }
};

template<typename T>
requires(!HasFormatter<T>)
struct Formatter<NonnullOwnPtr<T>> : Formatter<T const*> {
    ErrorOr<void> format(FormatBuilder& builder, NonnullOwnPtr<T> const& value)
    {
        return Formatter<T const*>::format(builder, value.ptr());
    }
};

}

#if USING_AK_GLOBALLY
using AK::adopt_nonnull_own_or_enomem;
using AK::adopt_own;
using AK::make;
using AK::NonnullOwnPtr;
using AK::try_make;
#endif
