/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibWeb/CSS/RustImportRule.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/CSS/URL.h>

namespace Web::CSS {

// Main-thread resource state for a native @import, shared by its sheet, pending requests, and
// any CSSOM wrapper. The parsed rule data remains owned by Rust. Owners trace its referenced
// GC objects through visit_edges().
class WEB_API StyleSheetImport final
    : public RefCounted<StyleSheetImport>
    , public Weakable<StyleSheetImport> {
public:
    static NonnullRefPtr<StyleSheetImport> create(RustRule, GC::Ptr<DOM::Document>);
    static NonnullRefPtr<StyleSheetImport> create(RustRuleView const&, GC::Ptr<DOM::Document>);

    u64 identity() const { return m_identity; }
    RustRule const& native_rule() const;
    URL const& url() const;
    Utf16String const& href() const { return url().url(); }
    RustMediaList const& native_media_list() const { return m_media_list; }
    GC::Ref<MediaList> media() const;

    StyleSheetState* parent_style_sheet() const { return m_parent_style_sheet.ptr(); }
    StyleSheetState* loaded_style_sheet() const { return m_style_sheet.ptr(); }
    void set_parent_style_sheet(StyleSheetState*);

    CSSImportRule& cssom_rule() const;
    void set_cssom_rule(CSSImportRule&);
    void visit_edges(GC::Cell::Visitor&);

    StyleSheetState::LoadingState loading_state() const { return m_loading_state; }
    void set_loading_state(StyleSheetState::LoadingState);

private:
    StyleSheetImport(u64 identity, RustImportRule, RustMediaList, GC::Ptr<DOM::Document>);
    void fetch();
    void set_style_sheet(NonnullRefPtr<StyleSheetState>);

    StyleSheetState::LoadingState m_loading_state { StyleSheetState::LoadingState::Unloaded };
    u64 m_identity;
    mutable Optional<RustRule> m_native_rule;
    RustImportRule m_rule;
    mutable Optional<URL> m_cached_url;
    GC::Ptr<DOM::Document> m_document;
    WeakPtr<StyleSheetState> m_parent_style_sheet;
    RustMediaList m_media_list;
    mutable GC::Weak<MediaList> m_media;
    RefPtr<StyleSheetState> m_style_sheet;
    mutable GC::Weak<CSSImportRule> m_cssom_rule;
};

}
