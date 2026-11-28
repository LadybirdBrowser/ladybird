/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGFitToViewBox.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

class SVGViewElement final : public SVGGraphicsElement
    , public SVGFitToViewBox {
    WEB_WRAPPABLE(SVGViewElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGViewElement);

public:
    virtual SVGFitToViewBox const* fit_to_view_box() const override { return this; }

private:
    SVGViewElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize_element() override;
    virtual void visit_edges(Visitor&) override;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;
};

}
