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
        : m_sheet(Parser::ValueParserFFI::rust_style_sheet_create(rules.handle(), media.handle()))
        , m_rules(Parser::ValueParserFFI::rust_style_sheet_rules(m_sheet), RustRuleList::Ownership::Borrow)
        , m_media(Parser::ValueParserFFI::rust_style_sheet_media(m_sheet), RustMediaList::Ownership::Borrow)
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
    // These are non-owning views into the Rust sheet. NativeStyleSheet owns the lists.
    Parser::ValueParserFFI::NativeStyleSheet const* m_sheet;
    RustRuleList m_rules;
    RustMediaList m_media;
};

}
