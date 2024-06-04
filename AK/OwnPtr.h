/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefCounted.h>

#define OWNPTR_SCRUB_BYTE 0xf0

namespace AK {

template<typename T, typename TDeleter>
class [[nodiscard]] OwnPtr {
public:
    OwnPtr() = default;

    OwnPtr(decltype(nullptr))
        : m_ptr(nullptr)
    {
    }

    OwnPtr(OwnPtr&& other)
        : m_ptr(other.leak_ptr())
    {
    }

    template<typename U>
    OwnPtr(NonnullOwnPtr<U>&& other)
        : m_ptr(other.leak_ptr())
    {
    }
    template<typename U>
    OwnPtr(OwnPtr<U>&& other)
        : m_ptr(other.leak_ptr())
    {
    }
    ~OwnPtr()
    {
        clear();
#ifdef SANITIZE_PTRS
        m_ptr = (T*)(explode_byte(OWNPTR_SCRUB_BYTE));
#endif
    }

    OwnPtr(OwnPtr const&) = delete;
    template<typename U>
    OwnPtr(OwnPtr<U> const&) = delete;
    OwnPtr& operator=(OwnPtr const&) = delete;
    template<typename U>
    OwnPtr& operator=(OwnPtr<U> const&) = delete;

    template<typename U>
    OwnPtr(NonnullOwnPtr<U> const&) = delete;
    template<typename U>
    OwnPtr& operator=(NonnullOwnPtr<U> const&) = delete;
    template<typename U>
    OwnPtr(RefPtr<U> const&) = delete;
    template<typename U>
    OwnPtr(NonnullRefPtr<U> const&) = delete;
    template<typename U>
    OwnPtr(WeakPtr<U> const&) = delete;
    template<typename U>
    OwnPtr& operator=(RefPtr<U> const&) = delete;
    template<typename U>
    OwnPtr& operator=(NonnullRefPtr<U> const&) = delete;
    template<typename U>
    OwnPtr& operator=(WeakPtr<U> const&) = delete;

    OwnPtr& operator=(OwnPtr&& other)
    {
        OwnPtr ptr(move(other));
        swap(ptr);
        return *this;
    }

    template<typename U>
    OwnPtr& operator=(OwnPtr<U>&& other)
    {
        OwnPtr ptr(move(other));
        swap(ptr);
        return *this;
    }

    template<typename U>
    OwnPtr& operator=(NonnullOwnPtr<U>&& other)
    {
        OwnPtr ptr(move(other));
        swap(ptr);
        VERIFY(m_ptr);
        return *this;
    }

    OwnPtr& operator=(T* ptr) = delete;

    OwnPtr& operator=(nullptr_t)
    {
        clear();
        return *this;
    }

    void clear()
    {
        auto* ptr = exchange(m_ptr, nullptr);
        TDeleter {}(ptr);
    }

    bool operator!() const { return !m_ptr; }

    [[nodiscard]] T* leak_ptr()
    {
        T* leaked_ptr = m_ptr;
        m_ptr = nullptr;
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

    T* ptr() const { return m_ptr; }

    T* operator->() const
    {
        VERIFY(m_ptr);
        return m_ptr;
    }

    T& operator*() const
    {
        VERIFY(m_ptr);
        return *m_ptr;
    }

    operator T*() const { return m_ptr; }

    operator bool() { return !!m_ptr; }

    void swap(OwnPtr& other)
    {
        AK::swap(m_ptr, other.m_ptr);
    }

    template<typename U>
    void swap(OwnPtr<U>& other)
    {
        AK::swap(m_ptr, other.m_ptr);
    }

    static OwnPtr lift(T* ptr)
    {
        return OwnPtr { ptr };
    }

protected:
    explicit OwnPtr(T* ptr)
        : m_ptr(ptr)
    {
        static_assert(
            requires { requires typename T::AllowOwnPtr()(); } || !requires { requires !typename T::AllowOwnPtr()(); declval<T>().ref(); declval<T>().unref(); }, "Use RefPtr<> for RefCounted types");
    }

private:
    T* m_ptr = nullptr;
};

template<typename T, typename U>
inline void swap(OwnPtr<T>& a, OwnPtr<U>& b)
{
    a.swap(b);
}

template<typename T>
inline OwnPtr<T> adopt_own_if_nonnull(T* object)
{
    if (object)
        return OwnPtr<T>::lift(object);
    return {};
}

template<typename T>
struct Traits<OwnPtr<T>> : public DefaultTraits<OwnPtr<T>> {
    using PeekType = T*;
    using ConstPeekType = T const*;
    static unsigned hash(OwnPtr<T> const& p) { return ptr_hash(p.ptr()); }
    static bool equals(OwnPtr<T> const& a, OwnPtr<T> const& b) { return a.ptr() == b.ptr(); }
};

template<typename T>
struct Formatter<OwnPtr<T>> : Formatter<T*> {
    ErrorOr<void> format(FormatBuilder& builder, OwnPtr<T> const& value)
    {
        return Formatter<T*>::format(builder, value.ptr());
    }
};
}

#if USING_AK_GLOBALLY
using AK::adopt_own_if_nonnull;
using AK::OwnPtr;
#endif
