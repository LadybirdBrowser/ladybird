/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#define NONNULLREFPTR_SCRUB_BYTE 0xe1

#include <AK/Assertions.h>
#include <AK/Format.h>
#include <AK/Traits.h>
#include <AK/Types.h>

namespace AK {

template<typename T>
class RefPtr;

template<typename T>
ALWAYS_INLINE void ref_if_not_null(T* ptr)
{
    if (ptr)
        ptr->ref();
}

template<typename T>
ALWAYS_INLINE void unref_if_not_null(T* ptr)
{
    if (ptr)
        ptr->unref();
}

template<typename T>
class [[nodiscard]] NonnullRefPtr {
    template<typename U>
    friend class RefPtr;
    template<typename U>
    friend class NonnullRefPtr;
    template<typename U>
    friend class WeakPtr;

public:
    using ElementType = T;

    enum AdoptTag { Adopt };

    ALWAYS_INLINE NonnullRefPtr(T& object)
        : m_ptr(&object)
    {
        m_ptr->ref();
    }

    template<typename U>
    requires(IsConvertible<U*, T*>)
    ALWAYS_INLINE NonnullRefPtr(U& object)
        : m_ptr(static_cast<T*>(&object))
    {
        m_ptr->ref();
    }

    ALWAYS_INLINE NonnullRefPtr(AdoptTag, T& object)
        : m_ptr(&object)
    {
    }

    ALWAYS_INLINE NonnullRefPtr(NonnullRefPtr&& other)
        : m_ptr(&other.leak_ref())
    {
    }

    template<typename U>
    requires(IsConvertible<U*, T*>)
    ALWAYS_INLINE NonnullRefPtr(NonnullRefPtr<U>&& other)
        : m_ptr(static_cast<T*>(&other.leak_ref()))
    {
    }

    ALWAYS_INLINE NonnullRefPtr(NonnullRefPtr const& other)
        : m_ptr(other.ptr())
    {
        m_ptr->ref();
    }

    template<typename U>
    requires(IsConvertible<U*, T*>)
    ALWAYS_INLINE NonnullRefPtr(NonnullRefPtr<U> const& other)
        : m_ptr(static_cast<T*>(other.ptr()))
    {
        m_ptr->ref();
    }

    ALWAYS_INLINE ~NonnullRefPtr()
    {
        auto* ptr = exchange(m_ptr, nullptr);
        unref_if_not_null(ptr);
#ifdef SANITIZE_PTRS
        m_ptr = reinterpret_cast<T*>(explode_byte(NONNULLREFPTR_SCRUB_BYTE));
#endif
    }

    template<typename U>
    NonnullRefPtr(OwnPtr<U> const&) = delete;
    template<typename U>
    NonnullRefPtr& operator=(OwnPtr<U> const&) = delete;

    template<typename U>
    NonnullRefPtr(RefPtr<U> const&) = delete;
    template<typename U>
    NonnullRefPtr& operator=(RefPtr<U> const&) = delete;
    NonnullRefPtr(RefPtr<T> const&) = delete;
    NonnullRefPtr& operator=(RefPtr<T> const&) = delete;

    NonnullRefPtr& operator=(NonnullRefPtr const& other)
    {
        NonnullRefPtr tmp { other };
        swap(tmp);
        return *this;
    }

    template<typename U>
    requires(IsConvertible<U*, T*>)
    NonnullRefPtr& operator=(NonnullRefPtr<U> const& other)
    {
        NonnullRefPtr tmp { other };
        swap(tmp);
        return *this;
    }

    ALWAYS_INLINE NonnullRefPtr& operator=(NonnullRefPtr&& other)
    {
        NonnullRefPtr tmp { move(other) };
        swap(tmp);
        return *this;
    }

    template<typename U>
    requires(IsConvertible<U*, T*>)
    NonnullRefPtr& operator=(NonnullRefPtr<U>&& other)
    {
        NonnullRefPtr tmp { move(other) };
        swap(tmp);
        return *this;
    }

    NonnullRefPtr& operator=(T& object)
    {
        NonnullRefPtr tmp { object };
        swap(tmp);
        return *this;
    }

    [[nodiscard]] ALWAYS_INLINE T& leak_ref()
    {
        T* ptr = exchange(m_ptr, nullptr);
        VERIFY(ptr);
        return *ptr;
    }

    ALWAYS_INLINE RETURNS_NONNULL T* ptr() const
    {
        return as_nonnull_ptr();
    }

    ALWAYS_INLINE RETURNS_NONNULL T* operator->() const
    {
        return as_nonnull_ptr();
    }

    [[nodiscard]] ALWAYS_INLINE T& operator*() const
    {
        return *as_nonnull_ptr();
    }

    ALWAYS_INLINE RETURNS_NONNULL operator T*() const
    {
        return as_nonnull_ptr();
    }

    ALWAYS_INLINE operator T&() const
    {
        return *as_nonnull_ptr();
    }

    operator bool() const = delete;
    bool operator!() const = delete;

    void swap(NonnullRefPtr& other)
    {
        AK::swap(m_ptr, other.m_ptr);
    }

    template<typename U>
    void swap(NonnullRefPtr<U>& other)
    requires(IsConvertible<U*, T*>)
    {
        AK::swap(m_ptr, other.m_ptr);
    }

    bool operator==(NonnullRefPtr const& other) const { return m_ptr == other.m_ptr; }

    template<typename RawPtr>
    requires(IsPointer<RawPtr>)
    bool operator==(RawPtr other) const
    {
        return m_ptr == other;
    }

private:
    NonnullRefPtr() = delete;

    ALWAYS_INLINE RETURNS_NONNULL T* as_nonnull_ptr() const
    {
        ASSERT(m_ptr);
        return m_ptr;
    }

    T* m_ptr { nullptr };
};

template<typename T>
inline NonnullRefPtr<T> adopt_ref(T& object)
{
    return NonnullRefPtr<T>(NonnullRefPtr<T>::Adopt, object);
}

// Use like `adopt_nonnull_ref_or_enomem(new (nothrow) T(args...))`.
template<typename T>
inline ErrorOr<NonnullRefPtr<T>> adopt_nonnull_ref_or_enomem(T* object)
{
    if (!object)
        return Error::from_errno(ENOMEM);
    return NonnullRefPtr<T>(NonnullRefPtr<T>::Adopt, *object);
}

template<typename T, class... Args>
requires(IsConstructible<T, Args...>) inline ErrorOr<NonnullRefPtr<T>> try_make_ref_counted(Args&&... args)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) T(forward<Args>(args)...));
}

template<Formattable T>
struct Formatter<NonnullRefPtr<T>> : Formatter<T> {
    ErrorOr<void> format(FormatBuilder& builder, NonnullRefPtr<T> const& value)
    {
        return Formatter<T>::format(builder, *value);
    }
};

template<typename T>
requires(!HasFormatter<T>)
struct Formatter<NonnullRefPtr<T>> : Formatter<T const*> {
    ErrorOr<void> format(FormatBuilder& builder, NonnullRefPtr<T> const& value)
    {
        return Formatter<T const*>::format(builder, value.ptr());
    }
};

template<typename T, typename U>
requires(IsConvertible<U*, T*>)
inline void swap(NonnullRefPtr<T>& a, NonnullRefPtr<U>& b)
{
    a.swap(b);
}

template<typename T, class... Args>
requires(IsConstructible<T, Args...>) inline NonnullRefPtr<T> make_ref_counted(Args&&... args)
{
    return NonnullRefPtr<T>(NonnullRefPtr<T>::Adopt, *new T(forward<Args>(args)...));
}

template<typename T>
struct Traits<NonnullRefPtr<T>> : public DefaultTraits<NonnullRefPtr<T>> {
    using PeekType = T*;
    using ConstPeekType = T const*;
    static unsigned hash(NonnullRefPtr<T> const& p) { return ptr_hash(p.ptr()); }
    static bool equals(NonnullRefPtr<T> const& a, NonnullRefPtr<T> const& b) { return a.ptr() == b.ptr(); }
};

}

#if USING_AK_GLOBALLY
using AK::adopt_nonnull_ref_or_enomem;
using AK::adopt_ref;
using AK::make_ref_counted;
using AK::NonnullRefPtr;
using AK::try_make_ref_counted;
#endif
