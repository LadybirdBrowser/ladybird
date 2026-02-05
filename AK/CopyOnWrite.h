/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>

namespace AK {

template<typename T>
class CopyOnWrite {
public:
    CopyOnWrite()
        : m_value(adopt_ref(*new T))
    {
    }
    T& mutable_value()
    {
        if (m_value->ref_count() > 1)
            m_value = m_value->clone();
        return *m_value;
    }
    T const& value() const { return *m_value; }

    operator T const&() const { return value(); }
    operator T&() { return mutable_value(); }

    T const* operator->() const { return &value(); }
    T* operator->() { return &mutable_value(); }

    T const* ptr() const { return m_value.ptr(); }
    T* ptr() { return m_value.ptr(); }

private:
    NonnullRefPtr<T> m_value;
};

}
