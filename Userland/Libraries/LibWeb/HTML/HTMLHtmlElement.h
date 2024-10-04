/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLHtmlElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLHtmlElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLHtmlElement);

public:
    virtual ~HTMLHtmlElement() override;

    bool should_use_body_background_properties() const;

    // https://www.w3.org/TR/html-aria/#el-html
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::document; }

private:
    HTMLHtmlElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual bool is_html_html_element() const override { return true; }
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLHtmlElement>() const { return is_html_html_element(); }
}
