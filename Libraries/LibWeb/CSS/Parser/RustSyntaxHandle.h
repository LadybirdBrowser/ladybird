/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/StdLibExtras.h>
#include <AK/Utf16String.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

// Shared handle to a Rust-allocated, immutable registered-syntax tree.
class RustSyntaxHandle {
public:
    explicit RustSyntaxHandle(void const* syntax)
        : m_syntax(syntax)
    {
        VERIFY(m_syntax);
    }

    RustSyntaxHandle(RustSyntaxHandle const& other)
        : m_syntax(other.m_syntax ? ValueParserFFI::rust_syntax_retain(other.m_syntax) : nullptr)
    {
    }

    RustSyntaxHandle& operator=(RustSyntaxHandle const& other)
    {
        RustSyntaxHandle copy(other);
        swap(m_syntax, copy.m_syntax);
        return *this;
    }

    RustSyntaxHandle(RustSyntaxHandle&& other)
        : m_syntax(exchange(other.m_syntax, nullptr))
    {
    }

    RustSyntaxHandle& operator=(RustSyntaxHandle&& other)
    {
        RustSyntaxHandle moved(move(other));
        swap(m_syntax, moved.m_syntax);
        return *this;
    }

    ~RustSyntaxHandle()
    {
        if (m_syntax)
            ValueParserFFI::rust_syntax_release(m_syntax);
    }

    static RustSyntaxHandle universal()
    {
        return RustSyntaxHandle { ValueParserFFI::rust_syntax_create_universal() };
    }

    bool is_universal() const { return ValueParserFFI::rust_syntax_is_universal(m_syntax); }
    bool is_single_component() const { return ValueParserFFI::rust_syntax_is_single_component(m_syntax); }
    Utf16String serialize() const { return Utf16String::adopt_raw(ValueParserFFI::rust_syntax_serialize(m_syntax)); }
    bool operator==(RustSyntaxHandle const& other) const
    {
        return m_syntax == other.m_syntax || ValueParserFFI::rust_syntax_equals(m_syntax, other.m_syntax);
    }

    void const* data() const { return m_syntax; }

private:
    void const* m_syntax { nullptr };
};

}
