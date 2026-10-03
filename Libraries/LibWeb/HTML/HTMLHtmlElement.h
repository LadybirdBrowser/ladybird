/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class WEB_API HTMLHtmlElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLHtmlElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLHtmlElement);

public:
    virtual ~HTMLHtmlElement() override;

    bool should_use_body_background_properties(Layout::BegunRead const& read) const;

    // Being the document's body is one of the facts a layout row is built with, and which child is the body moves
    // with this element's children. Called wherever they move.
    void publish_body_construction_facts();

    // https://www.w3.org/TR/html-aria/#el-html
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::document; }

private:
    HTMLHtmlElement(DOM::Document&, DOM::QualifiedName);
    virtual bool is_html_html_element() const override { return true; }
    virtual void children_changed(ChildrenChangedMetadata const&) override;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLHtmlElement>() const { return is_html_html_element(); }

}
