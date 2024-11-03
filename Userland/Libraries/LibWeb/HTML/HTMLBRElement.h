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
    JS_DECLARE_ALLOCATOR(HTMLBRElement);

public:
    virtual ~HTMLBRElement() override;

    virtual JS::GCPtr<Layout::Node> create_layout_node(CSS::StyleProperties) override;

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
