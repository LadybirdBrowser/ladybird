/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <stdlib.h>

namespace Web::CSS {

// A copy-on-write reference to a style value group struct.
//
// Copying a StyleStructRef shares the underlying payload by bumping a
// reference count instead of copying the struct. access() returns a mutable
// reference to the payload, cloning it first if it is shared with anyone
// else. Default-constructed refs share a single intentionally-leaked
// default-value payload per group type, so they cost no allocation.
//
// NB: This models the same copy-on-write scheme as WebKit's DataRef and
//     Blink's ComputedStyleBase groups. The payload layout (refcount header
//     followed by the value) deliberately matches the Arc layout that will
//     later be owned by Rust.
//
// Setting LIBWEB_STYLE_NO_STRUCT_SHARING in the environment makes copies
// deep-copy instead of sharing, to isolate sharing-related bugs and to
// measure the memory/performance effect of sharing.
template<typename T>
class StyleStructRef {
public:
    StyleStructRef()
        : m_payload(default_payload())
    {
        ++m_payload->ref_count;
    }

    StyleStructRef(StyleStructRef const& other)
    {
        if (sharing_disabled()) [[unlikely]] {
            m_payload = new Payload { 1, other.m_payload->value };
        } else {
            m_payload = other.m_payload;
            ++m_payload->ref_count;
        }
    }

    StyleStructRef& operator=(StyleStructRef const& other)
    {
        if (m_payload == other.m_payload)
            return *this;
        if (sharing_disabled()) [[unlikely]] {
            auto* clone = new Payload { 1, other.m_payload->value };
            deref();
            m_payload = clone;
        } else {
            ++other.m_payload->ref_count;
            deref();
            m_payload = other.m_payload;
        }
        return *this;
    }

    ~StyleStructRef()
    {
        deref();
    }

    T const& operator*() const { return m_payload->value; }
    T const* operator->() const { return &m_payload->value; }

    // Returns a mutable reference to the payload, cloning it first if it is
    // shared with anyone else (including the leaked default payload).
    T& access()
    {
        if (m_payload->ref_count > 1) {
            auto* clone = new Payload { 1, m_payload->value };
            --m_payload->ref_count;
            m_payload = clone;
        }
        return m_payload->value;
    }

    bool ptr_equals(StyleStructRef const& other) const { return m_payload == other.m_payload; }
    bool is_default() const { return m_payload == default_payload(); }

    static T const& default_value() { return default_payload()->value; }

    bool operator==(StyleStructRef const& other) const
    {
        return m_payload == other.m_payload || m_payload->value == other.m_payload->value;
    }

private:
    struct Payload {
        u64 ref_count { 0 };
        T value;
    };

    void deref()
    {
        if (--m_payload->ref_count == 0)
            delete m_payload;
    }

    static Payload* default_payload()
    {
        // NB: Seeded with one permanent reference and intentionally leaked, so
        //     the default payload is never destroyed and default construction
        //     never allocates. Groups whose initial computed values are not
        //     value-initialized members provide make_default_payload_value()
        //     so that the default payload matches what ComputedValues::create()
        //     produces for a completely unstyled element.
        static Payload* payload = [] {
            if constexpr (requires { T::make_default_payload_value(); })
                return new Payload { 1, T::make_default_payload_value() };
            else
                return new Payload { 1, T {} };
        }();
        return payload;
    }

    static bool sharing_disabled()
    {
        static bool disabled = getenv("LIBWEB_STYLE_NO_STRUCT_SHARING") != nullptr;
        return disabled;
    }

    Payload* m_payload { nullptr };
};

}
