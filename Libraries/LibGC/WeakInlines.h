/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Weak.h>

namespace GC {

template<typename T>
Weak<T>::Weak(T const* ptr)
    : m_impl(ptr ? *ptr->heap().create_weak_impl(const_cast<void*>(static_cast<void const*>(ptr))) : WeakImpl::the_null_weak_impl)
{
}

template<typename T>
Weak<T>::Weak(T const& ptr)
    : m_impl(*ptr.heap().create_weak_impl(const_cast<void*>(static_cast<void const*>(&ptr))))
{
}

template<typename T>
template<typename U>
Weak<T>::Weak(Weak<U> const& other)
requires(IsConvertible<U*, T*>)
    : m_impl(other.impl())
{
}

template<typename T>
Weak<T>::Weak(Ref<T> const& other)
    : m_impl(*other.ptr()->heap().create_weak_impl(other.ptr()))
{
}

template<typename T>
template<typename U>
Weak<T>::Weak(Ref<U> const& other)
requires(IsConvertible<U*, T*>)
    : m_impl(*other.ptr()->heap().create_weak_impl(other.ptr()))
{
}

template<typename T>
template<typename U>
Weak<T>& Weak<T>::operator=(U const& other)
requires(IsConvertible<U*, T*>)
{
    if (ptr() != other) {
        m_impl = *other.heap().create_weak_impl(const_cast<void*>(static_cast<void const*>(&other)));
    }
    return *this;
}

template<typename T>
Weak<T>& Weak<T>::operator=(Ref<T> const& other)
{
    if (ptr() != other.ptr()) {
        m_impl = *other.ptr()->heap().create_weak_impl(other.ptr());
    }
    return *this;
}

template<typename T>
template<typename U>
Weak<T>& Weak<T>::operator=(Ref<U> const& other)
requires(IsConvertible<U*, T*>)
{
    if (ptr() != other.ptr()) {
        m_impl = *other.ptr()->heap().create_weak_impl(other.ptr());
    }
    return *this;
}

template<typename T>
Weak<T>& Weak<T>::operator=(T const& other)
{
    if (ptr() != &other) {
        m_impl = *other.heap().create_weak_impl(const_cast<void*>(static_cast<void const*>(&other)));
    }
    return *this;
}

template<typename T>
Weak<T>& Weak<T>::operator=(T const* other)
{
    if (ptr() != other) {
        if (other)
            m_impl = *other->heap().create_weak_impl(const_cast<void*>(static_cast<void const*>(other)));
        else
            m_impl = WeakImpl::the_null_weak_impl;
    }
    return *this;
}

template<typename T>
template<typename U>
Weak<T>& Weak<T>::operator=(U const* other)
requires(IsConvertible<U*, T*>)
{
    if (ptr() != other) {
        if (other)
            m_impl = *other->heap().create_weak_impl(const_cast<void*>(static_cast<void const*>(other)));
        else
            m_impl = WeakImpl::the_null_weak_impl;
    }
    return *this;
}

}
