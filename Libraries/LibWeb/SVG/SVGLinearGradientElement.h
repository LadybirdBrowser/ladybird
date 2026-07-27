/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/SVG/SVGGradientElement.h>

namespace Web::SVG {

class SVGLinearGradientElement : public SVGGradientElement {
    WEB_PLATFORM_OBJECT(SVGLinearGradientElement, SVGGradientElement);
    GC_DECLARE_ALLOCATOR(SVGLinearGradientElement);

public:
    virtual ~SVGLinearGradientElement() override = default;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    virtual Optional<Painting::PaintStyle> to_gfx_paint_style(SVGPaintContext const&) const override;

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGLinearGradientElement__x1
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x1, Horizontal, CSS::PercentageStyleValue::create(CSS::Percentage { 0 }));

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGLinearGradientElement__y1
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y1, Vertical, CSS::PercentageStyleValue::create(CSS::Percentage { 0 }));

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGLinearGradientElement__x2
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x2, Horizontal, CSS::PercentageStyleValue::create(CSS::Percentage { 100 }));

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGLinearGradientElement__y2
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y2, Vertical, CSS::PercentageStyleValue::create(CSS::Percentage { 0 }));

protected:
    SVGLinearGradientElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

private:
    GC::Ptr<SVGLinearGradientElement const> linked_linear_gradient(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const
    {
        if (auto gradient = linked_gradient(seen_gradients); gradient && is<SVGLinearGradientElement>(*gradient))
            return &as<SVGLinearGradientElement>(*gradient);
        return {};
    }

    NumberPercentage start_x() const;
    NumberPercentage start_y() const;
    NumberPercentage end_x() const;
    NumberPercentage end_y() const;

    NumberPercentage start_x_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage start_y_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage end_x_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage end_y_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const;

    Optional<NumberPercentage> m_x1;
    Optional<NumberPercentage> m_y1;
    Optional<NumberPercentage> m_x2;
    Optional<NumberPercentage> m_y2;
};

}
