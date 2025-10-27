/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API StyleSheetList final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(StyleSheetList, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(StyleSheetList);

public:
    [[nodiscard]] static GC::Ref<StyleSheetList> create(GC::Ref<DOM::Node> document_or_shadow_root);

    void add_a_css_style_sheet(CSSStyleSheet&);
    void remove_a_css_style_sheet(CSSStyleSheet&);
    enum class Alternate : u8 {
        No,
        Yes,
    };
    enum class OriginClean : u8 {
        No,
        Yes,
    };
    GC::Ref<CSSStyleSheet> create_a_css_style_sheet(String const& css_text, String type, DOM::Element* owner_node, String const& media, String title, Alternate, OriginClean, Optional<::URL::URL> location, CSSStyleSheet* parent_style_sheet, CSSRule* owner_rule);

    Vector<GC::Ref<CSSStyleSheet>> const& sheets() const { return m_sheets; }
    Vector<GC::Ref<CSSStyleSheet>>& sheets() { return m_sheets; }

    CSSStyleSheet* item(size_t index) const
    {
        if (index >= m_sheets.size())
            return {};
        return const_cast<CSSStyleSheet*>(m_sheets[index].ptr());
    }

    size_t length() const { return m_sheets.size(); }

    virtual Optional<JS::Value> item_value(size_t index) const override;

    [[nodiscard]] DOM::Document& document();
    [[nodiscard]] DOM::Document const& document() const;

    [[nodiscard]] DOM::Node& document_or_shadow_root() { return m_document_or_shadow_root; }
    [[nodiscard]] DOM::Node const& document_or_shadow_root() const { return m_document_or_shadow_root; }

private:
    explicit StyleSheetList(GC::Ref<DOM::Node> document_or_shadow_root);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    void add_sheet(CSSStyleSheet&);
    void remove_sheet(CSSStyleSheet&);

    GC::Ref<DOM::Node> m_document_or_shadow_root;
    Vector<GC::Ref<CSSStyleSheet>> m_sheets;

    // https://www.w3.org/TR/cssom/#preferred-css-style-sheet-set-name
    String m_preferred_css_style_sheet_set_name;
    // https://www.w3.org/TR/cssom/#last-css-style-sheet-set-name
    Optional<String> m_last_css_style_sheet_set_name;
};

}
