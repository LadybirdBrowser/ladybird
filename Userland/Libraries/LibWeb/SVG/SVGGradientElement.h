/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IterationDecision.h>
#include <LibGfx/PaintStyle.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGStopElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

struct SVGPaintContext {
    Gfx::FloatRect viewport;
    Gfx::FloatRect path_bounding_box;
    Gfx::AffineTransform transform;
};

inline Painting::SVGGradientPaintStyle::SpreadMethod to_painting_spread_method(SpreadMethod spread_method)
{
    switch (spread_method) {
    case SpreadMethod::Pad:
        return Painting::SVGGradientPaintStyle::SpreadMethod::Pad;
    case SpreadMethod::Reflect:
        return Painting::SVGGradientPaintStyle::SpreadMethod::Reflect;
    case SpreadMethod::Repeat:
        return Painting::SVGGradientPaintStyle::SpreadMethod::Repeat;
    default:
        VERIFY_NOT_REACHED();
    }
}

class SVGGradientElement
    : public SVGElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes> {
    WEB_PLATFORM_OBJECT(SVGGradientElement, SVGElement);

public:
    virtual ~SVGGradientElement() override = default;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    virtual Optional<Painting::PaintStyle> to_gfx_paint_style(SVGPaintContext const&) const = 0;

    GradientUnits gradient_units() const;

    SpreadMethod spread_method() const;

    Optional<Gfx::AffineTransform> gradient_transform() const;

protected:
    SVGGradientElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    JS::GCPtr<SVGGradientElement const> linked_gradient(HashTable<SVGGradientElement const*>& seen_gradients) const;

    Gfx::AffineTransform gradient_paint_transform(SVGPaintContext const&) const;

    template<VoidFunction<SVGStopElement> Callback>
    void for_each_color_stop(Callback const& callback) const
    {
        HashTable<SVGGradientElement const*> seen_gradients;
        return for_each_color_stop_impl(callback, seen_gradients);
    }

    void add_color_stops(Painting::SVGGradientPaintStyle&) const;

private:
    template<VoidFunction<SVGStopElement> Callback>
    void for_each_color_stop_impl(Callback const& callback, HashTable<SVGGradientElement const*>& seen_gradients) const
    {
        bool color_stops_found = false;
        for_each_child_of_type<SVG::SVGStopElement>([&](auto& stop) {
            color_stops_found = true;
            callback(stop);
            return IterationDecision::Continue;
        });
        if (!color_stops_found) {
            if (auto gradient = linked_gradient(seen_gradients))
                gradient->for_each_color_stop_impl(callback, seen_gradients);
        }
    }

    GradientUnits gradient_units_impl(HashTable<SVGGradientElement const*>& seen_gradients) const;
    SpreadMethod spread_method_impl(HashTable<SVGGradientElement const*>& seen_gradients) const;
    Optional<Gfx::AffineTransform> gradient_transform_impl(HashTable<SVGGradientElement const*>& seen_gradients) const;

    Optional<GradientUnits> m_gradient_units = {};
    Optional<SpreadMethod> m_spread_method = {};
    Optional<Gfx::AffineTransform> m_gradient_transform = {};
};

}
