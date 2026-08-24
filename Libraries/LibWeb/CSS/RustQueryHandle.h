/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS {

class RustQueryHandle {
public:
    RustQueryHandle() = default;

    explicit RustQueryHandle(Parser::ValueParserFFI::FfiQueryHandle const* handle)
        : m_handle(handle)
    {
    }

    RustQueryHandle(RustQueryHandle const& other)
        : m_handle(Parser::ValueParserFFI::css_query_ref(other.m_handle))
    {
    }

    RustQueryHandle& operator=(RustQueryHandle const& other)
    {
        RustQueryHandle copy(other);
        swap(m_handle, copy.m_handle);
        return *this;
    }

    RustQueryHandle(RustQueryHandle&& other)
        : m_handle(exchange(other.m_handle, nullptr))
    {
    }

    RustQueryHandle& operator=(RustQueryHandle&& other)
    {
        RustQueryHandle moved(move(other));
        swap(m_handle, moved.m_handle);
        return *this;
    }

    static RustQueryHandle retained(Parser::ValueParserFFI::FfiQueryHandle const* handle)
    {
        return RustQueryHandle { Parser::ValueParserFFI::css_query_ref(handle) };
    }

    ~RustQueryHandle()
    {
        Parser::ValueParserFFI::css_query_unref(m_handle);
    }

    explicit operator bool() const { return m_handle; }
    Parser::ValueParserFFI::FfiQueryHandle const* data() const { return m_handle; }

private:
    Parser::ValueParserFFI::FfiQueryHandle const* m_handle { nullptr };
};

}
