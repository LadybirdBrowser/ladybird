/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGViewport.h>

namespace Web::SVG {

class SVGClipPathElement final : public SVGElement
    , public SVGViewport {
    WEB_PLATFORM_OBJECT(SVGClipPathElement, SVGElement);
    JS_DECLARE_ALLOCATOR(SVGClipPathElement);

public:
    virtual ~SVGClipPathElement();

    virtual Optional<ViewBox> view_box() const override
    {
        // Same trick as SVGMaskElement.
        if (clip_path_units() == MaskContentUnits::ObjectBoundingBox)
            return ViewBox { 0, 0, 1, 1 };
        return {};
    }

    virtual Optional<PreserveAspectRatio> preserve_aspect_ratio() const override
    {
        return PreserveAspectRatio { PreserveAspectRatio::Align::None, {} };
    }

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    ClipPathUnits clip_path_units() const
    {
        return m_clip_path_units.value_or(ClipPathUnits::UserSpaceOnUse);
    }

    virtual JS::GCPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::StyleProperties>) override;

private:
    SVGClipPathElement(DOM::Document&, DOM::QualifiedName);
    virtual void initialize(JS::Realm&) override;

    Optional<ClipPathUnits> m_clip_path_units = {};
};

}
