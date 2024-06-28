/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

class SVGAElement final : public SVGGraphicsElement {
    WEB_PLATFORM_OBJECT(SVGAElement, SVGGraphicsElement);
    JS_DECLARE_ALLOCATOR(SVGAElement);

public:
    virtual ~SVGAElement() override;

    virtual JS::GCPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::StyleProperties>) override;

private:
    SVGAElement(DOM::Document&, DOM::QualifiedName);
    virtual void initialize(JS::Realm&) override;
};

}
