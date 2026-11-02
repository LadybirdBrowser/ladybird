/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Utf16View.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS {

// The declaration blocks of a document's style attributes, by their text, so that elements that write the same text
// hold one block.
class RustInlineStyleBlocks {
    AK_MAKE_NONCOPYABLE(RustInlineStyleBlocks);
    AK_MAKE_NONMOVABLE(RustInlineStyleBlocks);

public:
    RustInlineStyleBlocks()
        : m_blocks(Parser::ValueParserFFI::rust_inline_style_blocks_create())
    {
    }

    ~RustInlineStyleBlocks() { Parser::ValueParserFFI::rust_inline_style_blocks_destroy(m_blocks); }

    // A new handle to the block of an element that wrote `text`, or null.
    Parser::ValueParserFFI::DeclarationBlock* find(Utf16View text, bool quirks) const
    {
        return Parser::ValueParserFFI::rust_inline_style_blocks_find(m_blocks, Parser::ffi_utf16_view(text), quirks);
    }

    void insert(Utf16View text, bool quirks, Parser::ValueParserFFI::DeclarationBlock const& block)
    {
        Parser::ValueParserFFI::rust_inline_style_blocks_insert(m_blocks, Parser::ffi_utf16_view(text), quirks, &block);
    }

private:
    Parser::ValueParserFFI::InlineStyleBlocks* m_blocks;
};

}
