/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/struct.html#InterfaceSVGMetadataElement
class SVGMetadataElement final : public SVGElement {
    WEB_PLATFORM_OBJECT(SVGMetadataElement, SVGElement);
    JS_DECLARE_ALLOCATOR(SVGMetadataElement);

private:
    SVGMetadataElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual JS::GCPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::StyleProperties>) override;
};

}
