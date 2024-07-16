/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Traits.h>
#include <AK/Types.h>

namespace JS {

template<typename T>
class GCPtr;

template<typename T>
class NonnullGCPtr {
public:
    NonnullGCPtr() = delete;

    NonnullGCPtr(T& ptr)
        : m_ptr(&ptr)
    {
    }

    template<typename U>
    NonnullGCPtr(U& ptr)
    requires(IsConvertible<U*, T*>)
        : m_ptr(&static_cast<T&>(ptr))
    {
    }

    template<typename U>
    NonnullGCPtr(NonnullGCPtr<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
    }

    template<typename U>
    NonnullGCPtr& operator=(NonnullGCPtr<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    NonnullGCPtr& operator=(T& other)
    {
        m_ptr = &other;
        return *this;
    }

    template<typename U>
    NonnullGCPtr& operator=(U& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = &static_cast<T&>(other);
        return *this;
    }

    RETURNS_NONNULL T* operator->() const { return m_ptr; }

    T& operator*() const { return *m_ptr; }

    RETURNS_NONNULL T* ptr() const { return m_ptr; }

    RETURNS_NONNULL operator T*() const { return m_ptr; }

    operator T&() const { return *m_ptr; }

private:
    T* m_ptr { nullptr };
};

template<typename T>
class GCPtr {
public:
    constexpr GCPtr() = default;

    GCPtr(T& ptr)
        : m_ptr(&ptr)
    {
    }

    GCPtr(T* ptr)
        : m_ptr(ptr)
    {
    }

    template<typename U>
    GCPtr(GCPtr<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
    }

    GCPtr(NonnullGCPtr<T> const& other)
        : m_ptr(other.ptr())
    {
    }

    template<typename U>
    GCPtr(NonnullGCPtr<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
    }

    GCPtr(nullptr_t)
        : m_ptr(nullptr)
    {
    }

    template<typename U>
    GCPtr& operator=(GCPtr<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    GCPtr& operator=(NonnullGCPtr<T> const& other)
    {
        m_ptr = other.ptr();
        return *this;
    }

    template<typename U>
    GCPtr& operator=(NonnullGCPtr<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    GCPtr& operator=(T& other)
    {
        m_ptr = &other;
        return *this;
    }

    template<typename U>
    GCPtr& operator=(U& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = &static_cast<T&>(other);
        return *this;
    }

    GCPtr& operator=(T* other)
    {
        m_ptr = other;
        return *this;
    }

    template<typename U>
    GCPtr& operator=(U* other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = static_cast<T*>(other);
        return *this;
    }

    T* operator->() const
    {
        ASSERT(m_ptr);
        return m_ptr;
    }

    T& operator*() const
    {
        ASSERT(m_ptr);
        return *m_ptr;
    }

    T* ptr() const { return m_ptr; }

    explicit operator bool() const { return !!m_ptr; }
    bool operator!() const { return !m_ptr; }

    operator T*() const { return m_ptr; }

private:
    T* m_ptr { nullptr };
};

// Non-Owning GCPtr
template<typename T>
using RawGCPtr = GCPtr<T>;

template<typename T, typename U>
inline bool operator==(GCPtr<T> const& a, GCPtr<U> const& b)
{
    return a.ptr() == b.ptr();
}

template<typename T, typename U>
inline bool operator==(GCPtr<T> const& a, NonnullGCPtr<U> const& b)
{
    return a.ptr() == b.ptr();
}

template<typename T, typename U>
inline bool operator==(NonnullGCPtr<T> const& a, NonnullGCPtr<U> const& b)
{
    return a.ptr() == b.ptr();
}

template<typename T, typename U>
inline bool operator==(NonnullGCPtr<T> const& a, GCPtr<U> const& b)
{
    return a.ptr() == b.ptr();
}

}

namespace AK {

template<typename T>
struct Traits<JS::GCPtr<T>> : public DefaultTraits<JS::GCPtr<T>> {
    static unsigned hash(JS::GCPtr<T> const& value)
    {
        return Traits<T*>::hash(value.ptr());
    }
};

template<typename T>
struct Traits<JS::NonnullGCPtr<T>> : public DefaultTraits<JS::NonnullGCPtr<T>> {
    static unsigned hash(JS::NonnullGCPtr<T> const& value)
    {
        return Traits<T*>::hash(value.ptr());
    }
};

}
