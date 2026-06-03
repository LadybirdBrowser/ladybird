/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class WEB_API CSSImportRule final
    : public CSSRule
    , public CSSStyleSheet::Subresource {
    WEB_PLATFORM_OBJECT(CSSImportRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSImportRule);

public:
    [[nodiscard]] static GC::Ref<CSSImportRule> create(JS::Realm&, URL, GC::Ptr<DOM::Document>, Optional<FlyString> layer, RefPtr<Supports>, GC::Ref<MediaList>);

    virtual ~CSSImportRule() override;

    URL const& url() const { return m_url; }
    String href() const { return m_url.url(); }

    CSSStyleSheet* loaded_style_sheet() { return m_style_sheet; }
    CSSStyleSheet const* loaded_style_sheet() const { return m_style_sheet; }
    GC::Ref<MediaList> media() const;
    CSSStyleSheet* style_sheet_for_bindings() { return m_style_sheet; }

    Optional<FlyString> layer_name() const;
    Optional<String> supports_text() const;

    bool matches() const;

    Optional<FlyString> internal_layer_name() const { return m_layer_internal; }
    Optional<FlyString> internal_qualified_layer_name(Badge<StyleScope>) const;

private:
    CSSImportRule(JS::Realm&, URL, GC::Ptr<DOM::Document>, Optional<FlyString>, RefPtr<Supports>, GC::Ref<MediaList>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    virtual void set_parent_style_sheet(CSSStyleSheet*) override;

    virtual GC::Ptr<CSSStyleSheet> parent_style_sheet_for_subresource() override { return m_parent_style_sheet; }

    virtual String serialized() const override;

    void fetch();
    void set_style_sheet(GC::Ref<CSSStyleSheet>);

    URL m_url;
    GC::Ptr<DOM::Document> m_document;
    Optional<FlyString> m_layer;
    Optional<FlyString> m_layer_internal;
    RefPtr<Supports> m_supports;
    GC::Ref<MediaList> m_media;
    GC::Ptr<CSSStyleSheet> m_style_sheet;
};

template<>
inline bool CSSRule::fast_is<CSSImportRule>() const { return type() == CSSRule::Type::Import; }

}
