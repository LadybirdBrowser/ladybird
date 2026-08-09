/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Noncopyable.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::CSS {

// Shared handle to a Rust-allocated, immutable StyleValueData. The StyleValueData layout is
// exposed through cbindgen, so reading through the handle is an inline field access with no FFI
// call.
class RustStyleValueHandle {
public:
    RustStyleValueHandle() = default;

    explicit RustStyleValueHandle(StyleValueFFI::StyleValueData const* value)
        : m_value(value)
    {
        VERIFY(m_value);
        StyleValueFFI::rust_style_ffi_note_style_value_created();
    }

    RustStyleValueHandle(RustStyleValueHandle const& other)
        : m_value(StyleValueFFI::rust_style_value_retain(other.m_value))
    {
    }

    RustStyleValueHandle& operator=(RustStyleValueHandle const& other)
    {
        RustStyleValueHandle copy(other);
        swap(m_value, copy.m_value);
        return *this;
    }

    RustStyleValueHandle(RustStyleValueHandle&& other)
        : m_value(exchange(other.m_value, nullptr))
    {
    }

    RustStyleValueHandle& operator=(RustStyleValueHandle&& other)
    {
        RustStyleValueHandle moved(move(other));
        swap(m_value, moved.m_value);
        return *this;
    }

    ~RustStyleValueHandle()
    {
        if (m_value)
            StyleValueFFI::rust_style_value_release(m_value);
    }

    StyleValueFFI::StyleValueData const& operator*() const { return *m_value; }
    StyleValueFFI::StyleValueData const* operator->() const { return m_value; }
    explicit operator bool() const { return m_value; }
    StyleValueFFI::StyleValueData const* data() const { return m_value; }

    StyleValueFFI::StyleValueData const* leak_data() { return exchange(m_value, nullptr); }

    // Two handles are the same value when they name the same allocation or hold equal values. A
    // defaulted comparison would only ever see the addresses, and a recomputed style builds a fresh
    // allocation for a declaration that has not changed.
    bool operator==(RustStyleValueHandle const& other) const
    {
        return StyleValueFFI::rust_style_value_equals(m_value, other.m_value);
    }

private:
    StyleValueFFI::StyleValueData const* m_value { nullptr };
};

}
