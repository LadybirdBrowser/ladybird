/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HyperlinkElementUtils.h>
#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLAnchorElement final
    : public MathMLElement
    , public HTML::HyperlinkElementUtils {
    WEB_PLATFORM_OBJECT(MathMLAnchorElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLAnchorElement);

public:
    virtual ~MathMLAnchorElement() override;

    String href() const;
    void set_href(String);

private:
    MathMLAnchorElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    // ^HTML::HTMLHyperlinkElementUtils
    virtual DOM::Element& hyperlink_element_utils_element() override { return *this; }
    virtual DOM::Element const& hyperlink_element_utils_element() const override { return *this; }
    virtual void update_href() override;
    virtual void set_the_url() override;
};

}
