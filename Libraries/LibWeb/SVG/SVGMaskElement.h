/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

class SVGMaskElement final : public SVGGraphicsElement {

    WEB_WRAPPABLE(SVGMaskElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGMaskElement);

public:
    virtual ~SVGMaskElement() override;

    virtual bool is_svg_mask_element() const final { return true; }

    virtual Optional<ViewBox> active_view_box() const override
    {
        // maskContentUnits = objectBoundingBox acts like the mask is sized to the bounding box
        // of the target element, with a viewBox of "0 0 1 1".
        if (mask_content_units() == MaskContentUnits::ObjectBoundingBox)
            return ViewBox { 0, 0, 1, 1 };
        return {};
    }

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    virtual Layout::Node* create_layout_node(CSS::LayoutStyle) override;

    CSSPixelRect resolve_masking_area(CSSPixelRect const& target_object_bounding_box, Gfx::FloatSize const& viewport_size, Gfx::AffineTransform const& user_space_to_css_pixels) const;

    MaskContentUnits mask_content_units() const;
    MaskUnits mask_units() const;
    NumberPercentage mask_x() const;
    NumberPercentage mask_y() const;
    NumberPercentage mask_width() const;
    NumberPercentage mask_height() const;

private:
    SVGMaskElement(DOM::Document&, DOM::QualifiedName);

    Optional<MaskContentUnits> m_mask_content_units = {};
    Optional<MaskUnits> m_mask_units = {};
    Optional<NumberPercentage> m_x = {};
    Optional<NumberPercentage> m_y = {};
    Optional<NumberPercentage> m_width = {};
    Optional<NumberPercentage> m_height = {};
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGMaskElement>() const { return is_svg_mask_element(); }

}
