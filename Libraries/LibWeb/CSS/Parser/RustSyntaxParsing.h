/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/DescriptorNameAndID.h>
#include <LibWeb/CSS/Parser/RuleContext.h>
#include <LibWeb/CSS/RustDeclarationBlock.h>
#include <LibWeb/CSS/RustDescriptorBlock.h>
#include <LibWeb/CSS/RustRule.h>
#include <LibWeb/Forward.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

class Parser;
struct ParsingParams;
struct DevToolsStyleDeclaration;

// A shared immutable Rust parse result can be transferred from a worker to the document thread.
class RustStyleSheetParse {
    AK_MAKE_NONCOPYABLE(RustStyleSheetParse);

public:
    explicit RustStyleSheetParse(ValueParserFFI::ParsedStyleSheet const* parse)
        : m_parse(parse)
    {
        VERIFY(m_parse);
    }
    RustStyleSheetParse(RustStyleSheetParse&& other)
        : m_parse(exchange(other.m_parse, nullptr))
    {
    }
    ~RustStyleSheetParse()
    {
        if (m_parse)
            ValueParserFFI::rust_css_syntax_parse_free(m_parse);
    }

    auto* handle() const { return m_parse; }
    RustRuleList native_rules() const;

private:
    ValueParserFFI::ParsedStyleSheet const* m_parse;
};

inline ValueParserFFI::FfiUtf16View ffi_utf16_view(Utf16View view)
{
    return {
        .ascii = view.has_ascii_storage() ? reinterpret_cast<u8 const*>(view.ascii_span().data()) : nullptr,
        .utf16 = view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(view.utf16_span().data()),
        .length = view.length_in_code_units(),
    };
}

}
