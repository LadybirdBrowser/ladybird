/*
 * Copyright (c) 2025, Ankit Khandelwal <ankk98@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/AffineTransform.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

class SVGPatternElement final : public SVGElement {
    WEB_PLATFORM_OBJECT(SVGPatternElement, SVGElement);

public:
    virtual ~SVGPatternElement() override = default;

    SVGPatternElement(DOM::Document&, DOM::QualifiedName);

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
    virtual void initialize(JS::Realm&) override;

    Optional<float> x() const { return m_x; }
    Optional<float> y() const { return m_y; }
    Optional<float> width() const { return m_width; }
    Optional<float> height() const { return m_height; }
    Optional<SVGUnits> pattern_units() const { return m_pattern_units; }
    Optional<SVGUnits> pattern_content_units() const { return m_pattern_content_units; }
    Optional<Gfx::AffineTransform> pattern_transform() const { return m_pattern_transform; }

private:
    Optional<float> m_x;
    Optional<float> m_y;
    Optional<float> m_width;
    Optional<float> m_height;
    Optional<SVGUnits> m_pattern_units;
    Optional<SVGUnits> m_pattern_content_units;
    Optional<Gfx::AffineTransform> m_pattern_transform;
};

}
