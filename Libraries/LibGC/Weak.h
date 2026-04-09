/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/RefPtr.h>
#include <LibGC/Export.h>
#include <LibGC/Ptr.h>

namespace GC {

class WeakBlock;

class WeakImpl {
public:
    // NOTE: Null GC::Weaks point at this WeakImpl. This allows Weak to always chase the impl pointer without null-checking it.
    static GC_API WeakImpl the_null_weak_impl;

    WeakImpl() = default;
    WeakImpl(void* ptr)
        : m_ptr(ptr)
    {
    }

    void* ptr() const { return m_ptr; }
    void set_ptr(Badge<WeakBlock>, void* ptr) { m_ptr = ptr; }

    bool operator==(WeakImpl const& other) const { return m_ptr == other.m_ptr; }
    bool operator!=(WeakImpl const& other) const { return m_ptr != other.m_ptr; }

    void ref() const { ++m_ref_count; }
    void unref() const
    {
        VERIFY(m_ref_count);
        --m_ref_count;
    }

    size_t ref_count() const { return m_ref_count; }

    enum class State {
        Allocated,
        Freelist,
    };

    void set_state(State state) { m_state = state; }
    State state() const { return m_state; }

private:
    mutable size_t m_ref_count { 0 };
    State m_state { State::Allocated };
    void* m_ptr { nullptr };
};

template<typename T>
class Weak {
public:
    constexpr Weak() = default;
    Weak(nullptr_t) { }

    Weak(T const* ptr);
    Weak(T const& ptr);

    Weak(Weak&& other)
        : m_impl(exchange(other.m_impl, WeakImpl::the_null_weak_impl))
    {
    }

    Weak& operator=(Weak&& other)
    {
        if (this != &other) {
            m_impl = exchange(other.m_impl, WeakImpl::the_null_weak_impl);
        }
        return *this;
    }

    Weak(Weak const&) = default;

    template<typename U>
    Weak(Weak<U> const& other)
    requires(IsConvertible<U*, T*>);

    Weak(Ref<T> const& other);

    template<typename U>
    Weak(Ref<U> const& other)
    requires(IsConvertible<U*, T*>);

    Weak& operator=(Weak const& other) = default;

    template<typename U>
    Weak& operator=(Weak<U> const& other)
    requires(IsConvertible<U*, T*>)
    {
        m_impl = other.impl();
        return *this;
    }

    Weak& operator=(Ref<T> const& other);

    template<typename U>
    Weak& operator=(Ref<U> const& other)
    requires(IsConvertible<U*, T*>);

    Weak& operator=(T const& other);

    template<typename U>
    Weak& operator=(U const& other)
    requires(IsConvertible<U*, T*>);

    Weak& operator=(T const* other);

    template<typename U>
    Weak& operator=(U const* other)
    requires(IsConvertible<U*, T*>);

    T* operator->() const
    {
        ASSERT(ptr());
        return ptr();
    }

    [[nodiscard]] T& operator*() const
    {
        ASSERT(ptr());
        return *ptr();
    }

    Ptr<T> ptr() const { return static_cast<T*>(impl().ptr()); }

    explicit operator bool() const { return !!ptr(); }
    bool operator!() const { return !ptr(); }

    operator T*() const { return ptr(); }

    Ref<T> as_nonnull() const
    {
        ASSERT(ptr());
        return *ptr();
    }

    WeakImpl& impl() const { return *m_impl; }

private:
    NonnullRefPtr<WeakImpl> m_impl { WeakImpl::the_null_weak_impl };
};

// NOTE: Unlike GC::Function, captures in this callback are not visited by the garbage collector.
//       Do not capture GC-managed objects without ensuring they are kept alive through other means.
template<typename T, typename Callback>
auto weak_callback(T& obj, Callback&& callback)
{
    return [weak = Weak<T> { obj }, cb = forward<Callback>(callback)](auto&&... args) mutable {
        if (weak)
            cb(*weak, forward<decltype(args)>(args)...);
    };
}

template<typename T, typename U>
inline bool operator==(Weak<T> const& a, Ptr<U> const& b)
{
    return a.ptr() == b.ptr();
}

template<typename T, typename U>
inline bool operator==(Weak<T> const& a, Ref<U> const& b)
{
    return a.ptr() == b.ptr();
}

}

namespace AK {

template<typename T>
struct Formatter<GC::Weak<T>> : Formatter<T const*> {
    ErrorOr<void> format(FormatBuilder& builder, GC::Weak<T> const& value)
    {
        return Formatter<T const*>::format(builder, value.ptr());
    }
};

}
