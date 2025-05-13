/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLTitleElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLTitleElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLTitleElement);

public:
    virtual ~HTMLTitleElement() override;

    String text() const;
    void set_text(String const& value);

private:
    HTMLTitleElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_title_element() const override { return true; }
    virtual void initialize(JS::Realm&) override;
    virtual void children_changed(ChildrenChangedMetadata const*) override;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLTitleElement>() const { return is_html_title_element(); }

}
