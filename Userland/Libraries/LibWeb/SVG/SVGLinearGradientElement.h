/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGGradientElement.h>

namespace Web::SVG {

class SVGLinearGradientElement : public SVGGradientElement {
    WEB_PLATFORM_OBJECT(SVGLinearGradientElement, SVGGradientElement);
    JS_DECLARE_ALLOCATOR(SVGLinearGradientElement);

public:
    virtual ~SVGLinearGradientElement() override = default;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    virtual Optional<Painting::PaintStyle> to_gfx_paint_style(SVGPaintContext const&) const override;

    JS::NonnullGCPtr<SVGAnimatedLength> x1() const;
    JS::NonnullGCPtr<SVGAnimatedLength> y1() const;
    JS::NonnullGCPtr<SVGAnimatedLength> x2() const;
    JS::NonnullGCPtr<SVGAnimatedLength> y2() const;

protected:
    SVGLinearGradientElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

private:
    JS::GCPtr<SVGLinearGradientElement const> linked_linear_gradient(HashTable<SVGGradientElement const*>& seen_gradients) const
    {
        if (auto gradient = linked_gradient(seen_gradients); gradient && is<SVGLinearGradientElement>(*gradient))
            return &verify_cast<SVGLinearGradientElement>(*gradient);
        return {};
    }

    NumberPercentage start_x() const;
    NumberPercentage start_y() const;
    NumberPercentage end_x() const;
    NumberPercentage end_y() const;

    NumberPercentage start_x_impl(HashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage start_y_impl(HashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage end_x_impl(HashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage end_y_impl(HashTable<SVGGradientElement const*>& seen_gradients) const;

    Optional<NumberPercentage> m_x1;
    Optional<NumberPercentage> m_y1;
    Optional<NumberPercentage> m_x2;
    Optional<NumberPercentage> m_y2;

    mutable RefPtr<Painting::SVGLinearGradientPaintStyle> m_paint_style;
};

}
