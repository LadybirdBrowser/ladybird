/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <LibGC/WriteBarrier.h>

namespace GC {

template<typename T>
class Ptr;

template<typename T>
class RawPtr;

template<typename T>
class RawRef;

template<typename T>
class Ref {
public:
    Ref() = delete;

    Ref(T& ptr)
        : m_ptr(&ptr)
    {
        write_barrier<T>(nullptr, m_ptr);
    }

    template<typename U>
    Ref(U& ptr)
    requires(IsConvertible<U*, T*>)
        : m_ptr(&static_cast<T&>(ptr))
    {
        write_barrier<T>(nullptr, m_ptr);
    }

    template<typename U>
    Ref(Ref<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
        write_barrier<T>(nullptr, m_ptr);
    }

    Ref(RawRef<T> const& other)
        : m_ptr(other.ptr())
    {
        write_barrier<T>(nullptr, m_ptr);
    }

    template<typename U>
    Ref(RawRef<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
        write_barrier<T>(nullptr, m_ptr);
    }

    template<typename U>
    Ref& operator=(Ref<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        write_barrier(m_ptr, static_cast<T*>(other.ptr()));
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    Ref& operator=(RawRef<T> const& other)
    {
        write_barrier(m_ptr, other.ptr());
        m_ptr = other.ptr();
        return *this;
    }

    template<typename U>
    Ref& operator=(RawRef<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        write_barrier(m_ptr, static_cast<T*>(other.ptr()));
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    Ref& operator=(T& other)
    {
        write_barrier(m_ptr, &other);
        m_ptr = &other;
        return *this;
    }

    template<typename U>
    Ref& operator=(U& other)
    requires(IsConvertible<U*, T*>)
    {
        write_barrier(m_ptr, &static_cast<T&>(other));
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
        write_barrier<T>(nullptr, m_ptr);
    }

    Ptr(T* ptr)
        : m_ptr(ptr)
    {
        write_barrier<T>(nullptr, m_ptr);
    }

    template<typename U>
    Ptr(Ptr<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
        write_barrier<T>(nullptr, m_ptr);
    }

    Ptr(Ref<T> const& other)
        : m_ptr(other.ptr())
    {
        write_barrier<T>(nullptr, m_ptr);
    }

    template<typename U>
    Ptr(Ref<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
        write_barrier<T>(nullptr, m_ptr);
    }

    Ptr(RawPtr<T> const& other)
        : m_ptr(other.ptr())
    {
        write_barrier<T>(nullptr, m_ptr);
    }

    template<typename U>
    Ptr(RawPtr<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
        write_barrier<T>(nullptr, m_ptr);
    }

    Ptr(RawRef<T> const& other)
        : m_ptr(other.ptr())
    {
        write_barrier<T>(nullptr, m_ptr);
    }

    template<typename U>
    Ptr(RawRef<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
        write_barrier<T>(nullptr, m_ptr);
    }

    Ptr(nullptr_t)
        : m_ptr(nullptr)
    {
    }

    template<typename U>
    Ptr& operator=(Ptr<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        write_barrier(m_ptr, static_cast<T*>(other.ptr()));
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    Ptr& operator=(Ref<T> const& other)
    {
        write_barrier(m_ptr, other.ptr());
        m_ptr = other.ptr();
        return *this;
    }

    template<typename U>
    Ptr& operator=(Ref<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        write_barrier(m_ptr, static_cast<T*>(other.ptr()));
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    Ptr& operator=(RawRef<T> const& other)
    {
        write_barrier(m_ptr, other.ptr());
        m_ptr = other.ptr();
        return *this;
    }

    template<typename U>
    Ptr& operator=(RawRef<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        write_barrier(m_ptr, static_cast<T*>(other.ptr()));
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    Ptr& operator=(RawPtr<T> const& other)
    {
        write_barrier(m_ptr, other.ptr());
        m_ptr = other.ptr();
        return *this;
    }

    template<typename U>
    Ptr& operator=(RawPtr<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        write_barrier(m_ptr, static_cast<T*>(other.ptr()));
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    Ptr& operator=(T& other)
    {
        write_barrier(m_ptr, &other);
        m_ptr = &other;
        return *this;
    }

    template<typename U>
    Ptr& operator=(U& other)
    requires(IsConvertible<U*, T*>)
    {
        write_barrier(m_ptr, &static_cast<T&>(other));
        m_ptr = &static_cast<T&>(other);
        return *this;
    }

    Ptr& operator=(T* other)
    {
        write_barrier(m_ptr, other);
        m_ptr = other;
        return *this;
    }

    template<typename U>
    Ptr& operator=(U* other)
    requires(IsConvertible<U*, T*>)
    {
        write_barrier(m_ptr, static_cast<T*>(other));
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

// Non-owning, untraced GC pointer. Not visited by visit_edges.
// Used for inline caches, weak bookkeeping, and other non-owning references.
// Unlike Ptr<T>, RawPtr<T> will not receive a write barrier when one is added.
template<typename T>
class RawPtr {
public:
    constexpr RawPtr() = default;

    RawPtr(T& ptr)
        : m_ptr(&ptr)
    {
    }

    RawPtr(T* ptr)
        : m_ptr(ptr)
    {
    }

    template<typename U>
    RawPtr(RawPtr<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
    }

    RawPtr(Ptr<T> const& other)
        : m_ptr(other.ptr())
    {
    }

    template<typename U>
    RawPtr(Ptr<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
    }

    RawPtr(Ref<T> const& other)
        : m_ptr(other.ptr())
    {
    }

    template<typename U>
    RawPtr(Ref<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
    }

    RawPtr(nullptr_t)
        : m_ptr(nullptr)
    {
    }

    template<typename U>
    RawPtr& operator=(RawPtr<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    RawPtr& operator=(Ptr<T> const& other)
    {
        m_ptr = other.ptr();
        return *this;
    }

    template<typename U>
    RawPtr& operator=(Ptr<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    RawPtr& operator=(Ref<T> const& other)
    {
        m_ptr = other.ptr();
        return *this;
    }

    template<typename U>
    RawPtr& operator=(Ref<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    RawPtr& operator=(T& other)
    {
        m_ptr = &other;
        return *this;
    }

    template<typename U>
    RawPtr& operator=(U& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = &static_cast<T&>(other);
        return *this;
    }

    RawPtr& operator=(T* other)
    {
        m_ptr = other;
        return *this;
    }

    template<typename U>
    RawPtr& operator=(U* other)
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

private:
    T* m_ptr { nullptr };
};

// Non-owning, untraced GC reference. Not visited by visit_edges.
// Unlike Ref<T>, RawRef<T> will not receive a write barrier when one is added.
template<typename T>
class RawRef {
public:
    RawRef() = delete;

    RawRef(T& ptr)
        : m_ptr(&ptr)
    {
    }

    template<typename U>
    RawRef(U& ptr)
    requires(IsConvertible<U*, T*>)
        : m_ptr(&static_cast<T&>(ptr))
    {
    }

    template<typename U>
    RawRef(RawRef<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
    }

    RawRef(Ref<T> const& other)
        : m_ptr(other.ptr())
    {
    }

    template<typename U>
    RawRef(Ref<U> const& other)
    requires(IsConvertible<U*, T*>)
        : m_ptr(other.ptr())
    {
    }

    template<typename U>
    RawRef& operator=(RawRef<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        m_ptr = static_cast<T*>(other.ptr());
        return *this;
    }

    RawRef& operator=(T& other)
    {
        m_ptr = &other;
        return *this;
    }

    template<typename U>
    RawRef& operator=(U& other)
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

template<typename T, typename U>
inline bool operator==(RawPtr<T> const& a, RawPtr<U> const& b)
{
    return a.ptr() == b.ptr();
}

template<typename T, typename U>
inline bool operator==(RawPtr<T> const& a, RawRef<U> const& b)
{
    return a.ptr() == b.ptr();
}

template<typename T, typename U>
inline bool operator==(RawRef<T> const& a, RawRef<U> const& b)
{
    return a.ptr() == b.ptr();
}

template<typename T, typename U>
inline bool operator==(RawRef<T> const& a, RawPtr<U> const& b)
{
    return a.ptr() == b.ptr();
}

// Cross-type comparisons between owning and non-owning pointers
template<typename T, typename U>
inline bool operator==(Ptr<T> const& a, RawPtr<U> const& b)
{
    return a.ptr() == b.ptr();
}

template<typename T, typename U>
inline bool operator==(RawPtr<T> const& a, Ptr<U> const& b)
{
    return a.ptr() == b.ptr();
}

template<typename T, typename U>
inline bool operator==(Ref<T> const& a, RawRef<U> const& b)
{
    return a.ptr() == b.ptr();
}

template<typename T, typename U>
inline bool operator==(RawRef<T> const& a, Ref<U> const& b)
{
    return a.ptr() == b.ptr();
}

}

namespace AK {

template<typename T>
struct Traits<GC::RawPtr<T>> : public DefaultTraits<GC::RawPtr<T>> {
    static unsigned hash(GC::RawPtr<T> const& value)
    {
        return Traits<T*>::hash(value.ptr());
    }
    static constexpr bool may_have_slow_equality_check() { return false; }
};

template<typename T>
struct Traits<GC::RawRef<T>> : public DefaultTraits<GC::RawRef<T>> {
    static unsigned hash(GC::RawRef<T> const& value)
    {
        return Traits<T*>::hash(value.ptr());
    }
};

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

template<typename T>
struct Formatter<GC::RawPtr<T>> : Formatter<T const*> {
    ErrorOr<void> format(FormatBuilder& builder, GC::RawPtr<T> const& value)
    {
        return Formatter<T const*>::format(builder, value.ptr());
    }
};

template<Formattable T>
struct Formatter<GC::RawRef<T>> : Formatter<T> {
    ErrorOr<void> format(FormatBuilder& builder, GC::RawRef<T> const& value)
    {
        return Formatter<T>::format(builder, *value);
    }
};

template<typename T>
requires(!HasFormatter<T>)
struct Formatter<GC::RawRef<T>> : Formatter<T const*> {
    ErrorOr<void> format(FormatBuilder& builder, GC::RawRef<T> const& value)
    {
        return Formatter<T const*>::format(builder, value.ptr());
    }
};

}
