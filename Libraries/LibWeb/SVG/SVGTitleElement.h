/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

class SVGTitleElement final : public SVGElement {
    WEB_PLATFORM_OBJECT(SVGTitleElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGTitleElement);

private:
    SVGTitleElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;
    virtual void children_changed(ChildrenChangedMetadata const*) override;
};

}
