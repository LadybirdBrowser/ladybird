/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/ImmutableBitmap.h>
#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/Painting/SVGMaskable.h>
#include <LibWeb/Painting/SVGPaintable.h>

namespace Web::Painting {

class SVGGraphicsPaintable : public SVGPaintable
    , public SVGMaskable {
    GC_CELL(SVGGraphicsPaintable, SVGPaintable);
    GC_DECLARE_ALLOCATOR(SVGGraphicsPaintable);

public:
    class ComputedTransforms {
    public:
        ComputedTransforms(Gfx::AffineTransform svg_to_viewbox_transform, Gfx::AffineTransform svg_transform)
            : m_svg_to_viewbox_transform(svg_to_viewbox_transform)
            , m_svg_transform(svg_transform)
        {
        }

        ComputedTransforms() = default;

        Gfx::AffineTransform const& svg_to_viewbox_transform() const { return m_svg_to_viewbox_transform; }
        Gfx::AffineTransform const& svg_transform() const { return m_svg_transform; }

        Gfx::AffineTransform svg_to_css_pixels_transform(
            Optional<Gfx::AffineTransform const&> additional_svg_transform = {}) const
        {
            return Gfx::AffineTransform {}.multiply(svg_to_viewbox_transform()).multiply(additional_svg_transform.value_or(Gfx::AffineTransform {})).multiply(svg_transform());
        }

        Gfx::AffineTransform svg_to_device_pixels_transform(DisplayListRecordingContext const& context) const
        {
            auto css_scale = context.device_pixels_per_css_pixel();
            return Gfx::AffineTransform {}.scale({ css_scale, css_scale }).multiply(svg_to_css_pixels_transform(context.svg_transform()));
        }

    private:
        Gfx::AffineTransform m_svg_to_viewbox_transform {};
        Gfx::AffineTransform m_svg_transform {};
    };

    static GC::Ref<SVGGraphicsPaintable> create(Layout::SVGGraphicsBox const&);

    virtual GC::Ptr<DOM::Node const> dom_node_of_svg() const override { return dom_node(); }
    virtual Optional<CSSPixelRect> get_mask_area() const override { return get_svg_mask_area(); }
    virtual Optional<Gfx::MaskKind> get_mask_type() const override { return get_svg_mask_type(); }
    virtual RefPtr<DisplayList> calculate_mask(DisplayListRecordingContext& context, CSSPixelRect const& mask_area) const override { return calculate_svg_mask_display_list(context, mask_area); }
    virtual Optional<CSSPixelRect> get_clip_area() const override { return get_svg_clip_area(); }
    virtual RefPtr<DisplayList> calculate_clip(DisplayListRecordingContext& context, CSSPixelRect const& clip_area) const override { return calculate_svg_clip_display_list(context, clip_area); }

    void set_computed_transforms(ComputedTransforms computed_transforms)
    {
        m_computed_transforms = computed_transforms;
    }

    ComputedTransforms const& computed_transforms() const
    {
        return m_computed_transforms;
    }

    virtual void reset_for_relayout() override;

protected:
    SVGGraphicsPaintable(Layout::SVGGraphicsBox const&);

    ComputedTransforms m_computed_transforms;

private:
    virtual bool is_svg_graphics_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<SVGGraphicsPaintable>() const { return is_svg_graphics_paintable(); }

}
