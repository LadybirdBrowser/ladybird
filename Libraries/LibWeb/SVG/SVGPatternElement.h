/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/SVG/AttributeParser.h>
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
    WEB_PLATFORM_OBJECT(SVGPatternElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGPatternElement);

public:
    virtual ~SVGPatternElement() override = default;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    SVGUnits pattern_units() const;
    SVGUnits pattern_content_units() const;
    Optional<Gfx::AffineTransform> pattern_transform() const;
    NumberPercentage pattern_x() const;
    NumberPercentage pattern_y() const;
    NumberPercentage pattern_width() const;
    NumberPercentage pattern_height() const;

    GC::Ref<SVGAnimatedLength> x() const;
    GC::Ref<SVGAnimatedLength> y() const;
    GC::Ref<SVGAnimatedLength> width() const;
    GC::Ref<SVGAnimatedLength> height() const;

    GC::Ptr<SVGPatternElement const> pattern_content_element() const;

    Optional<Painting::PaintStyle> to_gfx_paint_style(SVGPaintContext const&, DisplayListRecordingContext&, Layout::Node const& target_layout_node) const;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override { return nullptr; }

protected:
    SVGPatternElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    GC::Ptr<SVGPatternElement const> linked_pattern(HashTable<SVGPatternElement const*>& seen_patterns) const;
    GC::Ptr<SVGPatternElement const> pattern_content_element_impl(HashTable<SVGPatternElement const*>& seen_patterns) const;

    SVGUnits pattern_units_impl(HashTable<SVGPatternElement const*>& seen_patterns) const;
    SVGUnits pattern_content_units_impl(HashTable<SVGPatternElement const*>& seen_patterns) const;
    Optional<Gfx::AffineTransform> pattern_transform_impl(HashTable<SVGPatternElement const*>& seen_patterns) const;
    NumberPercentage pattern_x_impl(HashTable<SVGPatternElement const*>& seen_patterns) const;
    NumberPercentage pattern_y_impl(HashTable<SVGPatternElement const*>& seen_patterns) const;
    NumberPercentage pattern_width_impl(HashTable<SVGPatternElement const*>& seen_patterns) const;
    NumberPercentage pattern_height_impl(HashTable<SVGPatternElement const*>& seen_patterns) const;

    Optional<SVGUnits> m_pattern_units;
    Optional<SVGUnits> m_pattern_content_units;
    Optional<Gfx::AffineTransform> m_pattern_transform;
    Optional<NumberPercentage> m_x;
    Optional<NumberPercentage> m_y;
    Optional<NumberPercentage> m_width;
    Optional<NumberPercentage> m_height;
};

}
