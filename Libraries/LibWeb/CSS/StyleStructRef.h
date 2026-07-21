/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <LibWeb/CSS/RustStyleBridge.h>
#include <LibWeb/Export.h>
#include <stdlib.h>

namespace Web::CSS {

// NB: Must match STYLE_GROUP_STATIC_REFCOUNT in computed_values.rs.
static constexpr size_t style_group_static_refcount = NumericLimits<size_t>::max();

// Returns the intentionally leaked default payload for the given style group,
// registering all group vtables with the Rust ownership machinery on first use.
// Defined in ComputedValues.cpp, where the group types are visible.
WEB_API void const* style_group_default_payload(size_t group_index);

// A copy-on-write reference to a style value group struct.
//
// The payloads are owned by the Rust side of LibWeb (see computed_values.rs):
// Rust allocates, copies and destroys them through per-group vtable callbacks,
// and places an atomic reference count in a header immediately before each
// payload. Reading a group is an inline field access and sharing one is an
// inline atomic operation; only cloning for mutation and destroying the last
// reference cross the FFI boundary.
//
// Copying a StyleStructRef shares the underlying payload by bumping the
// reference count instead of copying the struct. access() returns a mutable
// reference to the payload, cloning it first if it is shared with anyone
// else. Default-constructed refs share a single intentionally-leaked
// default-value payload per group type, so they cost no allocation.
//
// NB: This models the same copy-on-write scheme as WebKit's DataRef and
//     Blink's ComputedStyleBase groups, with Stylo's arrangement of the
//     reference count header preceding the payload.
//
// Setting LIBWEB_STYLE_NO_STRUCT_SHARING in the environment makes copies
// deep-copy instead of sharing, to isolate sharing-related bugs and to
// measure the memory/performance effect of sharing.
template<typename T>
class StyleStructRef {
public:
    StyleStructRef()
        : m_payload(static_cast<T*>(const_cast<void*>(default_payload())))
    {
    }

    StyleStructRef(StyleStructRef const& other)
    {
        if (sharing_disabled()) [[unlikely]] {
            m_payload = clone_payload(other.m_payload);
        } else {
            m_payload = other.m_payload;
            ref();
        }
    }

    StyleStructRef& operator=(StyleStructRef const& other)
    {
        if (m_payload == other.m_payload)
            return *this;
        if (sharing_disabled()) [[unlikely]] {
            auto* clone = clone_payload(other.m_payload);
            deref();
            m_payload = clone;
        } else {
            other.ref();
            deref();
            m_payload = other.m_payload;
        }
        return *this;
    }

    ~StyleStructRef()
    {
        deref();
    }

    T const& operator*() const { return *m_payload; }
    T const* operator->() const { return m_payload; }

    // Returns a mutable reference to the payload, cloning it first if it is
    // shared with anyone else (including the leaked default payload).
    T& access()
    {
        if (refcount().load(AK::memory_order_acquire) != 1) {
            auto* clone = clone_payload(m_payload);
            deref();
            m_payload = clone;
        }
        return *m_payload;
    }

    // Takes ownership of a payload built on the Rust side; the payload
    // arrives already carrying this reference.
    void adopt(void* payload)
    {
        deref();
        m_payload = static_cast<T*>(payload);
    }

    bool ptr_equals(StyleStructRef const& other) const { return m_payload == other.m_payload; }
    bool is_default() const { return m_payload == default_payload(); }

    static T const& default_value() { return *static_cast<T const*>(default_payload()); }

    bool operator==(StyleStructRef const& other) const
    {
        return m_payload == other.m_payload || *m_payload == *other.m_payload;
    }

private:
    // NB: Must match header_size() in computed_values.rs.
    static constexpr size_t payload_header_size = max(alignof(T), sizeof(size_t));

    Atomic<size_t>& refcount() const
    {
        return *reinterpret_cast<Atomic<size_t>*>(reinterpret_cast<u8*>(m_payload) - payload_header_size);
    }

    void ref() const
    {
        auto& count = refcount();
        if (count.load(AK::memory_order_relaxed) == style_group_static_refcount)
            return;
        count.fetch_add(1, AK::memory_order_relaxed);
    }

    void deref()
    {
        auto& count = refcount();
        if (count.load(AK::memory_order_relaxed) == style_group_static_refcount)
            return;
        if (count.fetch_sub(1, AK::memory_order_acq_rel) == 1)
            free_rust_style_group(T::style_group_index, m_payload);
    }

    static T* clone_payload(T const* source)
    {
        return static_cast<T*>(clone_rust_style_group(T::style_group_index, source));
    }

    static void const* default_payload()
    {
        static void const* payload = style_group_default_payload(T::style_group_index);
        return payload;
    }

    static bool sharing_disabled()
    {
        static bool disabled = getenv("LIBWEB_STYLE_NO_STRUCT_SHARING") != nullptr;
        return disabled;
    }

    T* m_payload { nullptr };
};

}
