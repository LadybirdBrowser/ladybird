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

// Uniquely-owned handle to a Rust-allocated, immutable StyleValueData. Every StyleValue subclass
// keeps its data in a Rust-owned allocation, with this handle as the single owner on the C++
// side. The StyleValueData layout is exposed through cbindgen, so reading through the handle is
// an inline field access with no FFI call.
class RustStyleValueHandle {
    AK_MAKE_NONCOPYABLE(RustStyleValueHandle);

public:
    explicit RustStyleValueHandle(StyleValueFFI::StyleValueData* value)
        : m_value(value)
    {
        VERIFY(m_value);
        StyleValueFFI::rust_style_ffi_note_style_value_created();
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
            StyleValueFFI::rust_style_value_destroy(m_value);
    }

    StyleValueFFI::StyleValueData const& operator*() const { return *m_value; }
    StyleValueFFI::StyleValueData const* operator->() const { return m_value; }

private:
    StyleValueFFI::StyleValueData* m_value { nullptr };
};

}
