/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS {

class RustPageSelectors {
public:
    explicit RustPageSelectors(Parser::ValueParserFFI::PageSelectorList const* selectors)
        : m_selectors(Parser::ValueParserFFI::rust_page_selector_list_retain(selectors))
    {
        VERIFY(m_selectors);
    }
    RustPageSelectors(RustPageSelectors const& other)
        : m_selectors(Parser::ValueParserFFI::rust_page_selector_list_retain(other.m_selectors))
    {
    }
    RustPageSelectors(RustPageSelectors&& other)
        : m_selectors(exchange(other.m_selectors, nullptr))
    {
    }
    RustPageSelectors& operator=(RustPageSelectors const& other)
    {
        RustPageSelectors copy(other);
        swap(m_selectors, copy.m_selectors);
        return *this;
    }
    RustPageSelectors& operator=(RustPageSelectors&& other)
    {
        RustPageSelectors moved(move(other));
        swap(m_selectors, moved.m_selectors);
        return *this;
    }
    ~RustPageSelectors() { Parser::ValueParserFFI::rust_page_selector_list_release(m_selectors); }

    static Optional<RustPageSelectors> parse(Utf16View text)
    {
        auto* selectors = Parser::ValueParserFFI::rust_parse_page_selector_list({
            .ascii = text.has_ascii_storage() ? reinterpret_cast<u8 const*>(text.ascii_span().data()) : nullptr,
            .utf16 = text.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(text.utf16_span().data()),
            .length = text.length_in_code_units(),
        });
        if (!selectors)
            return {};
        RustPageSelectors result { selectors };
        Parser::ValueParserFFI::rust_page_selector_list_release(selectors);
        return result;
    }

    Parser::ValueParserFFI::PageSelectorList const* handle() const { return m_selectors; }
    size_t size() const { return Parser::ValueParserFFI::rust_page_selector_list_count(m_selectors); }
    Utf16String serialize() const
    {
        Utf16String result;
        Parser::ValueParserFFI::rust_page_selector_list_serialize(m_selectors, &result, [](void* context, u16 const* data, size_t length) {
            *static_cast<Utf16String*>(context) = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(data), length });
        });
        return result;
    }

private:
    Parser::ValueParserFFI::PageSelectorList const* m_selectors;
};

}
