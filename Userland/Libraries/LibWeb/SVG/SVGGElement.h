/*
 * Copyright (c) 2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

class SVGGElement final : public SVGGraphicsElement {
    WEB_PLATFORM_OBJECT(SVGGElement, SVGGraphicsElement);
    JS_DECLARE_ALLOCATOR(SVGGElement);

public:
    virtual ~SVGGElement() override = default;

    virtual JS::GCPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::StyleProperties>) override;

private:
    SVGGElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
