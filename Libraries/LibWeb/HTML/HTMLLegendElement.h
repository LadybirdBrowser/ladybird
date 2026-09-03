/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLLegendElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLLegendElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLLegendElement);

public:
    virtual ~HTMLLegendElement() override;

    HTMLFormElement* form();

    virtual Layout::Node* create_layout_node(CSS::LayoutStyle) override;

private:
    HTMLLegendElement(DOM::Document&, DOM::QualifiedName);
};

}
