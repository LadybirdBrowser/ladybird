/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

class SVGDescElement final : public SVGElement {
    WEB_PLATFORM_OBJECT(SVGDescElement, SVGElement);
    JS_DECLARE_ALLOCATOR(SVGDescElement);

private:
    SVGDescElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual JS::GCPtr<Layout::Node> create_layout_node(CSS::StyleProperties) override;
};

}
