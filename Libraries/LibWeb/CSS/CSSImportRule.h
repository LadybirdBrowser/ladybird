/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API CSSImportRule final
    : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSImportRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSImportRule);

public:
    [[nodiscard]] static GC::Ref<CSSImportRule> create(JS::Realm&, URL, GC::Ptr<DOM::Document>, RefPtr<Supports>, Vector<NonnullRefPtr<MediaQuery>>);

    virtual ~CSSImportRule() = default;

    URL const& url() const { return m_url; }
    String href() const { return m_url.url(); }

    CSSStyleSheet* loaded_style_sheet() { return m_style_sheet; }
    CSSStyleSheet const* loaded_style_sheet() const { return m_style_sheet; }
    GC::Ptr<MediaList> media() const;
    CSSStyleSheet* style_sheet_for_bindings() { return m_style_sheet; }

    Optional<String> supports_text() const;

private:
    CSSImportRule(JS::Realm&, URL, GC::Ptr<DOM::Document>, RefPtr<Supports>, Vector<NonnullRefPtr<MediaQuery>>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void set_parent_style_sheet(CSSStyleSheet*) override;

    virtual String serialized() const override;

    void fetch();
    void set_style_sheet(GC::Ref<CSSStyleSheet>);

    URL m_url;
    GC::Ptr<DOM::Document> m_document;
    RefPtr<Supports> m_supports;
    Vector<NonnullRefPtr<MediaQuery>> m_media_query_list;
    GC::Ptr<CSSStyleSheet> m_style_sheet;
    Optional<DOM::DocumentLoadEventDelayer> m_document_load_event_delayer;
};

template<>
inline bool CSSRule::fast_is<CSSImportRule>() const { return type() == CSSRule::Type::Import; }

}
