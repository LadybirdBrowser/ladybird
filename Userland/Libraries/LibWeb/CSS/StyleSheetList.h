/*
 * Copyright (c) 2020-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/CSSStyleSheet.h>

namespace Web::CSS {

class StyleSheetList final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(StyleSheetList, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(StyleSheetList);

public:
    [[nodiscard]] static JS::NonnullGCPtr<StyleSheetList> create(DOM::Document&);

    void add_a_css_style_sheet(CSS::CSSStyleSheet&);
    void remove_a_css_style_sheet(CSS::CSSStyleSheet&);
    void create_a_css_style_sheet(String type, DOM::Element* owner_node, String media, String title, bool alternate, bool origin_clean, Optional<String> location, CSS::CSSStyleSheet* parent_style_sheet, CSS::CSSRule* owner_rule, CSS::CSSStyleSheet&);

    Vector<JS::NonnullGCPtr<CSSStyleSheet>> const& sheets() const { return m_sheets; }
    Vector<JS::NonnullGCPtr<CSSStyleSheet>>& sheets() { return m_sheets; }

    CSSStyleSheet* item(size_t index) const
    {
        if (index >= m_sheets.size())
            return {};
        return const_cast<CSSStyleSheet*>(m_sheets[index].ptr());
    }

    size_t length() const { return m_sheets.size(); }

    virtual bool is_supported_property_index(u32 index) const override;
    virtual Optional<JS::Value> item_value(size_t index) const override;

    DOM::Document& document() { return m_document; }
    DOM::Document const& document() const { return m_document; }

private:
    explicit StyleSheetList(DOM::Document&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    void add_sheet(CSSStyleSheet&);
    void remove_sheet(CSSStyleSheet&);

    JS::NonnullGCPtr<DOM::Document> m_document;
    Vector<JS::NonnullGCPtr<CSSStyleSheet>> m_sheets;

    // https://www.w3.org/TR/cssom/#preferred-css-style-sheet-set-name
    String m_preferred_css_style_sheet_set_name;
    // https://www.w3.org/TR/cssom/#last-css-style-sheet-set-name
    Optional<String> m_last_css_style_sheet_set_name;
};

}
