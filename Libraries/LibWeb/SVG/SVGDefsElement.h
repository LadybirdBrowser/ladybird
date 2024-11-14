/*
 * Copyright (c) 2022, Simon Danner <danner.simon@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

class SVGDefsElement final : public SVGGraphicsElement {
    WEB_PLATFORM_OBJECT(SVGDefsElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGDefsElement);

public:
    virtual ~SVGDefsElement();

    virtual GC::Ptr<Layout::Node> create_layout_node(CSS::StyleProperties) override
    {
        return nullptr;
    }

private:
    SVGDefsElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
