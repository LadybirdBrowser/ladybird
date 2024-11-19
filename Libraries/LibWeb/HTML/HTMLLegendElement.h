/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/Layout/LegendBox.h>

namespace Web::HTML {

class HTMLLegendElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLLegendElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLLegendElement);

public:
    virtual ~HTMLLegendElement() override;

    HTMLFormElement* form();

    virtual GC::Ptr<Layout::Node> create_layout_node(CSS::StyleProperties) override;
    Layout::LegendBox* layout_node();
    Layout::LegendBox const* layout_node() const;

private:
    HTMLLegendElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
