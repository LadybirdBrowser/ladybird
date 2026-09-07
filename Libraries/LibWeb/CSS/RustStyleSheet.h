/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/RustMediaList.h>
#include <LibWeb/CSS/RustRule.h>

namespace Web::CSS {

class RustStyleSheet {
    AK_MAKE_NONCOPYABLE(RustStyleSheet);
    AK_MAKE_NONMOVABLE(RustStyleSheet);

public:
    using Flag = Parser::ValueParserFFI::NativeStyleSheetFlag;
    using MediaState = Parser::ValueParserFFI::NativeStyleSheetMediaState;

    RustStyleSheet(RustRuleList rules, RustMediaList media)
        : m_rules(move(rules))
        , m_media(move(media))
        , m_sheet(Parser::ValueParserFFI::rust_style_sheet_create(m_rules.handle(), m_media.handle()))
    {
    }
    ~RustStyleSheet() { Parser::ValueParserFFI::rust_style_sheet_release(m_sheet); }

    RustRuleList const& rules() const { return m_rules; }
    RustRuleList& rules() { return m_rules; }
    RustMediaList const& media() const { return m_media; }
    MediaState media_state() const { return Parser::ValueParserFFI::rust_style_sheet_media_state(m_sheet); }
    void reset_media_state() const { Parser::ValueParserFFI::rust_style_sheet_reset_media_state(m_sheet); }
    void set_import(u64 rule_identity, RustStyleSheet const* imported) const
    {
        Parser::ValueParserFFI::rust_style_sheet_set_import(m_sheet, rule_identity, imported ? imported->handle() : nullptr);
    }
    bool flag(Flag flag) const { return Parser::ValueParserFFI::rust_style_sheet_flag(m_sheet, flag); }
    void set_flag(Flag flag, bool value) { Parser::ValueParserFFI::rust_style_sheet_set_flag(m_sheet, flag, value); }
    Parser::ValueParserFFI::NativeStyleSheet const* handle() const { return m_sheet; }

private:
    // Retain direct handles for hot rule/media traversal without allocating temporary FFI handles.
    // The Rust sheet also owns these lists and can outlive this facade.
    RustRuleList m_rules;
    RustMediaList m_media;
    Parser::ValueParserFFI::NativeStyleSheet const* m_sheet;
};

}
