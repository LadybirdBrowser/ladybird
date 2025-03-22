/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>

namespace Web::CSS {

class CSSImportRule final
    : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSImportRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSImportRule);

public:
    [[nodiscard]] static GC::Ref<CSSImportRule> create(URL::URL, DOM::Document&, RefPtr<Supports>);

    virtual ~CSSImportRule() = default;

    URL::URL const& url() const { return m_url; }
    // FIXME: This should return only the specified part of the url. eg, "stuff/foo.css", not "https://example.com/stuff/foo.css".
    String href() const { return m_url.to_string(); }

    CSSStyleSheet* loaded_style_sheet() { return m_style_sheet; }
    CSSStyleSheet const* loaded_style_sheet() const { return m_style_sheet; }
    CSSStyleSheet* style_sheet_for_bindings() { return m_style_sheet; }

    Optional<String> supports_text() const;

private:
    CSSImportRule(URL::URL, DOM::Document&, RefPtr<Supports>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void set_parent_style_sheet(CSSStyleSheet*) override;

    virtual String serialized() const override;

    void fetch();
    void set_style_sheet(GC::Ref<CSSStyleSheet>);

    URL::URL m_url;
    GC::Ptr<DOM::Document> m_document;
    RefPtr<Supports> m_supports;
    GC::Ptr<CSSStyleSheet> m_style_sheet;
    Optional<DOM::DocumentLoadEventDelayer> m_document_load_event_delayer;
};

template<>
inline bool CSSRule::fast_is<CSSImportRule>() const { return type() == CSSRule::Type::Import; }

}
