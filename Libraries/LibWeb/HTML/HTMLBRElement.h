/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLBRElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLBRElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLBRElement);

public:
    virtual ~HTMLBRElement() override;

    virtual RefPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::ComputedValues const>) override;
    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;
    virtual void adjust_computed_style(CSS::ComputedProperties::Builder&) override;

    // Whether this <br> renders an empty line, i.e. nothing else renders between the start of its line and the <br>
    // itself. Such a <br> hosts a caret position on its parent, at its child index.
    bool represents_empty_line() const;

private:
    virtual bool is_html_br_element() const override { return true; }

    HTMLBRElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLBRElement>() const { return is_html_br_element(); }

}
