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

    virtual CSS::ElementBoxKind box_kind() const override;

    MaskContentUnits mask_content_units() const;
    MaskUnits mask_units() const;
    NumberPercentage mask_x() const;
    NumberPercentage mask_y() const;
    NumberPercentage mask_width() const;
    NumberPercentage mask_height() const;

    GC::Ref<SVGAnimatedEnumeration> mask_units_for_bindings();
    GC::Ref<SVGAnimatedEnumeration> mask_content_units_for_bindings();

    // https://drafts.fxtf.org/css-masking/#dom-svgmaskelement-x
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x, Horizontal, SVGLengthValue::percentage(-10));

    // https://drafts.fxtf.org/css-masking/#dom-svgmaskelement-y
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y, Vertical, SVGLengthValue::percentage(-10));

    // https://drafts.fxtf.org/css-masking/#dom-svgmaskelement-width
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(width, Horizontal, SVGLengthValue::percentage(120));

    // https://drafts.fxtf.org/css-masking/#dom-svgmaskelement-height
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(height, Vertical, SVGLengthValue::percentage(120));

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
