/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Traits.h>
#include <AK/Types.h>

namespace GC {

template<typename T>
class Ptr;

template<typename T>
class Ref {
public:
    Ref() = delete;

    Ref(T& ptr)
        : m_ptr(&ptr)
    {
    }

    template<typename U>
    Ref(U& ptr)
    requires(IsConvertible<U*, T*>)
        : m_ptr(&static_cast<T&>(ptr))
    {
    }

    template<typename U>
    Ref(Ref<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
    }

    template<typename U>
    Ref& operator=(Ref<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    Ref& operator=(T& other)
    {
        m_ptr = &other;
        return *this;
    }

    template<typename U>
    Ref& operator=(U& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = &static_cast<T&>(other);
        return *this;
    }

    RETURNS_NONNULL T* operator->() const { return m_ptr; }

    [[nodiscard]] T& operator*() const { return *m_ptr; }

    RETURNS_NONNULL T* ptr() const { return m_ptr; }

    RETURNS_NONNULL operator T*() const { return m_ptr; }

    operator T&() const { return *m_ptr; }

    operator bool() const = delete;
    bool operator!() const = delete;

private:
    T* m_ptr { nullptr };
};

template<typename T>
class Ptr {
public:
    constexpr Ptr() = default;

    Ptr(T& ptr)
        : m_ptr(&ptr)
    {
    }

    Ptr(T* ptr)
        : m_ptr(ptr)
    {
    }

    template<typename U>
    Ptr(Ptr<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
    }

    Ptr(Ref<T> const& other)
        : m_ptr(other.ptr())
    {
    }

    template<typename U>
    Ptr(Ref<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
    }

    Ptr(nullptr_t)
        : m_ptr(nullptr)
    {
    }

    template<typename U>
    Ptr& operator=(Ptr<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    Ptr& operator=(Ref<T> const& other)
    {
        m_ptr = other.ptr();
        return *this;
    }

    template<typename U>
    Ptr& operator=(Ref<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    Ptr& operator=(T& other)
    {
        m_ptr = &other;
        return *this;
    }

    template<typename U>
    Ptr& operator=(U& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = &static_cast<T&>(other);
        return *this;
    }

    Ptr& operator=(T* other)
    {
        m_ptr = other;
        return *this;
    }

    template<typename U>
    Ptr& operator=(U* other)
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

    [[nodiscard]] T& operator*() const
    {
        ASSERT(m_ptr);
        return *m_ptr;
    }

    T* ptr() const { return m_ptr; }

    explicit operator bool() const { return !!m_ptr; }
    bool operator!() const { return !m_ptr; }

    operator T*() const { return m_ptr; }

    Ref<T> as_nonnull() const
    {
        VERIFY(m_ptr);
        return *m_ptr;
    }

private:
    T* m_ptr { nullptr };
};

// Non-Owning GC::Ptr
template<typename T>
using RawPtr = Ptr<T>;

// Non-Owning Ref
template<typename T>
using RawRef = Ref<T>;

template<typename T, typename U>
inline bool operator==(Ptr<T> const& a, Ptr<U> const& b)
{
    return a.ptr() == b.ptr();
}

template<typename T, typename U>
inline bool operator==(Ptr<T> const& a, Ref<U> const& b)
{
    return a.ptr() == b.ptr();
}

template<typename T, typename U>
inline bool operator==(Ref<T> const& a, Ref<U> const& b)
{
    return a.ptr() == b.ptr();
}

template<typename T, typename U>
inline bool operator==(Ref<T> const& a, Ptr<U> const& b)
{
    return a.ptr() == b.ptr();
}

}

namespace AK {

template<typename T>
struct Traits<GC::Ptr<T>> : public DefaultTraits<GC::Ptr<T>> {
    static unsigned hash(GC::Ptr<T> const& value)
    {
        return Traits<T*>::hash(value.ptr());
    }
    static constexpr bool may_have_slow_equality_check() { return false; }
};

template<typename T>
struct Traits<GC::Ref<T>> : public DefaultTraits<GC::Ref<T>> {
    static unsigned hash(GC::Ref<T> const& value)
    {
        return Traits<T*>::hash(value.ptr());
    }
};

template<typename T>
struct Formatter<GC::Ptr<T>> : Formatter<T const*> {
    ErrorOr<void> format(FormatBuilder& builder, GC::Ptr<T> const& value)
    {
        return Formatter<T const*>::format(builder, value.ptr());
    }
};

template<Formattable T>
struct Formatter<GC::Ref<T>> : Formatter<T> {
    ErrorOr<void> format(FormatBuilder& builder, GC::Ref<T> const& value)
    {
        return Formatter<T>::format(builder, *value);
    }
};

template<typename T>
requires(!HasFormatter<T>)
struct Formatter<GC::Ref<T>> : Formatter<T const*> {
    ErrorOr<void> format(FormatBuilder& builder, GC::Ref<T> const& value)
    {
        return Formatter<T const*>::format(builder, value.ptr());
    }
};

}
