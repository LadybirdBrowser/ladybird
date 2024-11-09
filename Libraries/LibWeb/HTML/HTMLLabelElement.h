/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLLabelElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLLabelElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLLabelElement);

public:
    virtual ~HTMLLabelElement() override;

    virtual JS::GCPtr<Layout::Node> create_layout_node(CSS::StyleProperties) override;

    Optional<String> for_() const { return attribute(HTML::AttributeNames::for_); }

    JS::GCPtr<HTMLElement> control() const;
    JS::GCPtr<HTMLFormElement> form() const;

private:
    HTMLLabelElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
