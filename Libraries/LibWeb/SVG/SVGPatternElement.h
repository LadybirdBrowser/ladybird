/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/RootHashTable.h>
#include <LibGfx/Matrix4x4.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFitToViewBox.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

class SVGPatternElement
    : public SVGElement
    , public SVGFitToViewBox
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes> {
    WEB_WRAPPABLE(SVGPatternElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGPatternElement);

public:
    virtual ~SVGPatternElement() override = default;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    SVGUnits pattern_units() const;
    SVGUnits pattern_content_units() const;
    Optional<Gfx::AffineTransform> pattern_transform() const;
    NumberPercentage pattern_x() const;
    NumberPercentage pattern_y() const;
    NumberPercentage pattern_width() const;
    NumberPercentage pattern_height() const;

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGPatternElement__x
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x, Horizontal, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGPatternElement__y
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y, Vertical, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGPatternElement__width
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(width, Horizontal, SVGLengthValue::number(0));

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGPatternElement__height
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(height, Vertical, SVGLengthValue::number(0));

    GC::Ptr<SVGPatternElement const> pattern_content_element() const;

    struct PaintGeometry {
        Layout::NodeWithStyle const* pattern_layout_node;
        Gfx::FloatRect tile_rect;
        Gfx::FloatSize content_scale;
        Gfx::FloatMatrix4x4 tile_content_transform;
        Optional<Gfx::AffineTransform> device_pattern_transform;
    };
    Optional<PaintGeometry> resolve_paint_geometry(SVGPaintContext const&, double device_pixels_per_css_pixel, Layout::Node const& target_layout_node) const;

    virtual RefPtr<Layout::Node> create_layout_node(CSS::LayoutStyle) override { return nullptr; }

protected:
    SVGPatternElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize_element() override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    virtual bool is_svg_pattern_element() const final { return true; }

    GC::Ptr<SVGPatternElement const> linked_pattern(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const;
    GC::Ptr<SVGPatternElement const> pattern_content_element_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const;

    SVGUnits pattern_units_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const;
    SVGUnits pattern_content_units_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const;
    Optional<Gfx::AffineTransform> pattern_transform_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const;
    NumberPercentage pattern_x_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const;
    NumberPercentage pattern_y_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const;
    NumberPercentage pattern_width_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const;
    NumberPercentage pattern_height_impl(GC::RootHashTable<SVGPatternElement const*>& seen_patterns) const;

    Optional<SVGUnits> m_pattern_units;
    Optional<SVGUnits> m_pattern_content_units;
    Optional<Gfx::AffineTransform> m_pattern_transform;
    Optional<NumberPercentage> m_x;
    Optional<NumberPercentage> m_y;
    Optional<NumberPercentage> m_width;
    Optional<NumberPercentage> m_height;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGPatternElement>() const { return is_svg_pattern_element(); }

}
