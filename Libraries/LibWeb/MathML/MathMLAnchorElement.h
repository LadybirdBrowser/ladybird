/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLHyperlinkElementUtils.h>
#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

// AD-HOC: MathML Core's set the url steps do not set the url to null first, so href would keep returning the old URL
//         after the href attribute is removed. This shares the HTML steps instead, which do.
class MathMLAnchorElement final
    : public MathMLElement
    , public HTML::HTMLHyperlinkElementUtils {
    WEB_WRAPPABLE(MathMLAnchorElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLAnchorElement);

public:
    virtual ~MathMLAnchorElement() override;

private:
    MathMLAnchorElement(DOM::Document&, DOM::QualifiedName);

    // ^HTML::HTMLHyperlinkElementUtils
    virtual DOM::Element& hyperlink_element_utils_element() override { return *this; }
    virtual DOM::Element const& hyperlink_element_utils_element() const override { return *this; }
};

}
