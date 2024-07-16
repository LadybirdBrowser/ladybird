/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Bitmap.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::SVG {

class SVGPathElement final : public SVGGeometryElement {
    WEB_PLATFORM_OBJECT(SVGPathElement, SVGGeometryElement);
    JS_DECLARE_ALLOCATOR(SVGPathElement);

public:
    virtual ~SVGPathElement() override = default;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size) override;

private:
    SVGPathElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    Vector<PathInstruction> m_instructions;
};

Gfx::Path path_from_path_instructions(ReadonlySpan<PathInstruction>);

}
