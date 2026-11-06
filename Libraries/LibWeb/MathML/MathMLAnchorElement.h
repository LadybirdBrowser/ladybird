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

    virtual Optional<URL::Origin> extract_an_origin() const override { return hyperlink_element_utils_extract_an_origin(); }

    virtual bool is_mathml_anchor_element() const override { return true; }

    // ^EventTarget
    virtual bool is_focusable() const override;

private:
    MathMLAnchorElement(DOM::Document&, DOM::QualifiedName);

    // ^DOM::EventTarget
    virtual bool has_activation_behavior() const override;
    virtual void activation_behavior(DOM::Event const&) override;

    // ^DOM::Element
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;
    virtual i32 default_tab_index_value() const override;

    // ^HTML::HTMLHyperlinkElementUtils
    virtual DOM::Element& hyperlink_element_utils_element() override { return *this; }
    virtual DOM::Element const& hyperlink_element_utils_element() const override { return *this; }
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<MathML::MathMLAnchorElement>() const { return is_mathml_anchor_element(); }

}
