/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Weakable.h>

namespace AK {

template<typename T>
class [[nodiscard]] WeakPtr {
    template<typename U>
    friend class Weakable;

public:
    WeakPtr() = default;

    template<SameAs<OptionalNone> V>
    WeakPtr(V) { }

    template<SameAs<OptionalNone> V>
    WeakPtr& operator=(V)
    {
        clear();
        return *this;
    }

    template<typename U>
    WeakPtr(WeakPtr<U> const& other)
    requires(IsBaseOf<T, U>)
        : m_link(other.m_link)
    {
    }

    template<typename U>
    WeakPtr(WeakPtr<U>&& other)
    requires(IsBaseOf<T, U>)
        : m_link(other.take_link())
    {
    }

    template<typename U>
    WeakPtr& operator=(WeakPtr<U>&& other)
    requires(IsBaseOf<T, U>)
    {
        m_link = other.take_link();
        return *this;
    }

    template<typename U>
    WeakPtr& operator=(WeakPtr<U> const& other)
    requires(IsBaseOf<T, U>)
    {
        if ((void const*)this != (void const*)&other)
            m_link = other.m_link;
        return *this;
    }

    WeakPtr& operator=(nullptr_t)
    {
        clear();
        return *this;
    }

    template<typename U>
    WeakPtr(U const& object)
    requires(IsBaseOf<T, U>)
        : m_link(object.template make_weak_ptr<U>().take_link())
    {
    }

    template<typename U>
    WeakPtr(U const* object)
    requires(IsBaseOf<T, U>)
    {
        if (object)
            m_link = object->template make_weak_ptr<U>().take_link();
    }

    template<typename U>
    WeakPtr(RefPtr<U> const& object)
    requires(IsBaseOf<T, U>)
    {
        if (object)
            m_link = object->template make_weak_ptr<U>().take_link();
    }

    template<typename U>
    WeakPtr(NonnullRefPtr<U> const& object)
    requires(IsBaseOf<T, U>)
    {
        m_link = object->template make_weak_ptr<U>().take_link();
    }

    template<typename U>
    WeakPtr& operator=(U const& object)
    requires(IsBaseOf<T, U>)
    {
        m_link = object.template make_weak_ptr<U>().take_link();
        return *this;
    }

    template<typename U>
    WeakPtr& operator=(U const* object)
    requires(IsBaseOf<T, U>)
    {
        if (object)
            m_link = object->template make_weak_ptr<U>().take_link();
        else
            m_link = nullptr;
        return *this;
    }

    template<typename U>
    WeakPtr& operator=(RefPtr<U> const& object)
    requires(IsBaseOf<T, U>)
    {
        if (object)
            m_link = object->template make_weak_ptr<U>().take_link();
        else
            m_link = nullptr;
        return *this;
    }

    template<typename U>
    WeakPtr& operator=(NonnullRefPtr<U> const& object)
    requires(IsBaseOf<T, U>)
    {
        m_link = object->template make_weak_ptr<U>().take_link();
        return *this;
    }

    [[nodiscard]] RefPtr<T> strong_ref() const
    {
        return RefPtr<T> { ptr() };
    }

    T* ptr() const { return unsafe_ptr(); }
    T* operator->() const { return unsafe_ptr(); }
    operator T*() const { return unsafe_ptr(); }

    [[nodiscard]] T* unsafe_ptr() const
    {
        if (m_link)
            return m_link->template unsafe_ptr<T>();
        return nullptr;
    }

    [[nodiscard]] NonnullRefPtr<T> value() const
    {
        VERIFY(has_value());
        return *unsafe_ptr();
    }

    operator bool() const { return m_link ? !m_link->is_null() : false; }

    [[nodiscard]] bool is_null() const { return !m_link || m_link->is_null(); }
    [[nodiscard]] bool has_value() const { return !is_null(); }

    void clear() { m_link = nullptr; }

    [[nodiscard]] RefPtr<WeakLink> take_link() { return move(m_link); }

private:
    WeakPtr(RefPtr<WeakLink> const& link)
        : m_link(link)
    {
    }

    RefPtr<WeakLink> m_link;
};

template<typename T>
template<typename U>
inline ErrorOr<WeakPtr<U>> Weakable<T>::try_make_weak_ptr() const
{
    if (!m_link)
        m_link = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) WeakLink(const_cast<T&>(static_cast<T const&>(*this)))));

    return WeakPtr<U>(m_link);
}

template<typename T>
struct Formatter<WeakPtr<T>> : Formatter<T const*> {
    ErrorOr<void> format(FormatBuilder& builder, WeakPtr<T> const& value)
    {
        return Formatter<T const*>::format(builder, value.ptr());
    }
};

template<typename T>
ErrorOr<WeakPtr<T>> try_make_weak_ptr_if_nonnull(T const* ptr)
{
    if (ptr) {
        return ptr->template try_make_weak_ptr<T>();
    }
    return WeakPtr<T> {};
}

template<typename T>
WeakPtr<T> make_weak_ptr_if_nonnull(T const* ptr)
{
    return MUST(try_make_weak_ptr_if_nonnull(ptr));
}

template<typename T>
struct Traits<WeakPtr<T>> : public DefaultTraits<WeakPtr<T>> {
    using PeekType = T*;
    using ConstPeekType = T const*;
    static unsigned hash(WeakPtr<T> const& p) { return ptr_hash(p.ptr()); }
    static bool equals(WeakPtr<T> const& a, WeakPtr<T> const& b) { return a.ptr() == b.ptr(); }
};

}

#if USING_AK_GLOBALLY
using AK::WeakPtr;
#endif
