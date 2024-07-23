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
    void update_a_style_block(DOM::Element& style_element, JS::GCPtr<DOM::Node> old_parent_if_removed_from = nullptr);

    CSS::CSSStyleSheet* sheet() { return m_associated_css_style_sheet; }
    CSS::CSSStyleSheet const* sheet() const { return m_associated_css_style_sheet; }

private:
    // https://www.w3.org/TR/cssom/#associated-css-style-sheet
    JS::GCPtr<CSS::CSSStyleSheet> m_associated_css_style_sheet;
};

}
