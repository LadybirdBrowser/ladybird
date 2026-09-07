/*
 * Copyright (c) 2019-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleSheet.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom-1/#cssstylesheet
class WEB_API CSSStyleSheet final : public StyleSheet {
    WEB_WRAPPABLE(CSSStyleSheet, StyleSheet);
    GC_DECLARE_ALLOCATOR(CSSStyleSheet);

public:
    virtual ~CSSStyleSheet() override;

    static WebIDL::ExceptionOr<GC::Ref<CSSStyleSheet>> create_for_constructor(JS::Object&, CSSStyleSheetOptions const& options = {});

    GC::Ptr<CSSRule> owner_rule() { return state().owner_rule(); }
    CSSRuleList* css_rules() { return &state().rules(); }

    WebIDL::ExceptionOr<unsigned> insert_rule(Utf16View rule, unsigned index) { return state().insert_rule(rule, index); }
    WebIDL::ExceptionOr<WebIDL::Long> add_rule(Optional<Utf16String> selector, Optional<Utf16String> style, Optional<WebIDL::UnsignedLong> index) { return state().add_rule(move(selector), move(style), index); }
    WebIDL::ExceptionOr<void> remove_rule(Optional<WebIDL::UnsignedLong> index) { return state().remove_rule(index); }
    WebIDL::ExceptionOr<void> delete_rule(unsigned index) { return state().delete_rule(index); }
    GC::Ref<WebIDL::Promise> replace(Utf16String text) { return state().replace(move(text)); }
    WebIDL::ExceptionOr<void> replace_sync(Utf16View text) { return state().replace_sync(text); }

private:
    friend class StyleSheetState;
    [[nodiscard]] static GC::Ref<CSSStyleSheet> create(StyleSheetState&);
    explicit CSSStyleSheet(StyleSheetState&);
    virtual void visit_edges(GC::Cell::Visitor&) override;
    void update_owner_chain();

    GC::Ptr<CSSStyleSheet> m_parent_sheet;
    RefPtr<StyleSheetImport> m_owner_import;
};

}
