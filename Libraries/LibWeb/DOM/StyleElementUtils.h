/*
 * Copyright (c) 2023, Preston Taylor <PrestonLeeTaylor@proton.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/WebIDL/ObservableArray.h>

namespace Web::DOM {

class StyleElementUtils {
public:
    void update_a_style_block(DOM::Element& style_element);

    CSS::CSSStyleSheet* sheet() { return m_associated_css_style_sheet; }
    CSS::CSSStyleSheet const* sheet() const { return m_associated_css_style_sheet; }

    [[nodiscard]] GC::Ptr<CSS::StyleSheetList> style_sheet_list() { return m_style_sheet_list; }
    [[nodiscard]] GC::Ptr<CSS::StyleSheetList const> style_sheet_list() const { return m_style_sheet_list; }

    void visit_edges(JS::Cell::Visitor&);

private:
    // https://www.w3.org/TR/cssom/#associated-css-style-sheet
    GC::Ptr<CSS::CSSStyleSheet> m_associated_css_style_sheet;

    GC::Ptr<CSS::StyleSheetList> m_style_sheet_list;
};

}
