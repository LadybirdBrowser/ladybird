/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <stdlib.h>

namespace Web::CSS {

// A reference to a style value group struct.
//
// The payloads are owned by the Rust side of LibWeb (see computed_values.rs),
// which defines each group's layout and lifecycle. Rust places an atomic
// reference count in a header immediately before each payload. Reading a
// group is an inline field access and sharing one is an inline atomic operation;
// only cloning for mutation and destroying the last reference cross the FFI
// boundary.
//
// Copying a StyleStructRef shares the underlying payload by bumping the
// reference count instead of copying the struct. A default-constructed ref is
// empty until a payload is borrowed or assigned.
template<typename T>
class StyleStructRef {
public:
    // NB: An empty reference is borrowed, so that it is filled by borrow() or assignment before its
    //     payload is read.
    StyleStructRef() = default;

    StyleStructRef(StyleStructRef const& other)
        : m_payload(other.m_payload)
        , m_is_borrowed(false)
    {
        ref();
    }

    StyleStructRef& operator=(StyleStructRef const& other)
    {
        if (this == &other || (m_payload == other.m_payload && !m_is_borrowed))
            return *this;
        other.ref();
        deref();
        m_payload = other.m_payload;
        m_is_borrowed = false;
        return *this;
    }

    ~StyleStructRef()
    {
        deref();
    }

    T const& operator*() const { return *m_payload; }
    T const* operator->() const { return m_payload; }

    // Borrows a payload retained by a live StyleRecord. Copying this reference
    // first acquires its own payload, so the borrowed state never escapes the
    // synchronous record view which created it.
    void borrow(void const* payload)
    {
        deref();
        m_payload = static_cast<T*>(const_cast<void*>(payload));
        m_is_borrowed = true;
    }

    bool ptr_equals(StyleStructRef const& other) const { return m_payload == other.m_payload; }

    // Whether the payload holds the group's initial values.
    bool is_default() const { return ComputedValuesFFI::rust_style_group_holds_default_values(T::style_group_index, m_payload); }

private:
    // NB: Must match header_size() in computed_values.rs.
    static constexpr size_t payload_header_size = max(alignof(T), sizeof(size_t));

    Atomic<size_t>& refcount() const
    {
        return *reinterpret_cast<Atomic<size_t>*>(reinterpret_cast<u8*>(m_payload) - payload_header_size);
    }

    void ref() const
    {
        VERIFY(m_payload);
        refcount().fetch_add(1, AK::memory_order_relaxed);
    }

    void deref()
    {
        if (m_is_borrowed)
            return;
        if (refcount().fetch_sub(1, AK::memory_order_acq_rel) == 1)
            ComputedValuesFFI::rust_style_group_free(T::style_group_index, m_payload);
    }

    T* m_payload { nullptr };
    bool m_is_borrowed { true };
};

}
