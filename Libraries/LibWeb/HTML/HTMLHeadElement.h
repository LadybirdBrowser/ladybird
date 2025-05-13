/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLHeadElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLHeadElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLHeadElement);

public:
    virtual ~HTMLHeadElement() override;

private:
    HTMLHeadElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_head_element() const final { return true; }
    virtual void initialize(JS::Realm&) override;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLHeadElement>() const { return is_html_head_element(); }

}
