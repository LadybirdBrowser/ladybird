/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <glib-object.h>

namespace Ladybird {

// RAII wrapper for GObject-derived pointers. Calls g_object_unref on destruction.
// Does not add a reference on construction — assumes ownership of a floating or
// newly-created reference.
template<typename T>
class GObjectPtr {
public:
    GObjectPtr() = default;
    explicit GObjectPtr(T* ptr)
        : m_ptr(ptr)
    {
    }

    ~GObjectPtr()
    {
        clear();
    }

    GObjectPtr(GObjectPtr const&) = delete;
    GObjectPtr& operator=(GObjectPtr const&) = delete;

    GObjectPtr(GObjectPtr&& other)
        : m_ptr(other.leak())
    {
    }

    GObjectPtr& operator=(GObjectPtr&& other)
    {
        if (this != &other) {
            clear();
            m_ptr = other.leak();
        }
        return *this;
    }

    T* ptr() const { return m_ptr; }
    operator T*() const { return m_ptr; }

    T* leak()
    {
        auto* ptr = m_ptr;
        m_ptr = nullptr;
        return ptr;
    }

    void clear()
    {
        if (m_ptr) {
            g_object_unref(m_ptr);
            m_ptr = nullptr;
        }
    }

private:
    T* m_ptr { nullptr };
};

}
