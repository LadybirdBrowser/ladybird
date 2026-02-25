/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/Layout/VideoBox.h>
#include <LibWeb/Painting/VideoPaintable.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(VideoBox);

VideoBox::VideoBox(DOM::Document& document, DOM::Element& element, GC::Ref<CSS::ComputedProperties> style)
    : ReplacedBox(document, element, style)
{
}

HTML::HTMLVideoElement& VideoBox::dom_node()
{
    return static_cast<HTML::HTMLVideoElement&>(*ReplacedBox::dom_node());
}

HTML::HTMLVideoElement const& VideoBox::dom_node() const
{
    return static_cast<HTML::HTMLVideoElement const&>(*ReplacedBox::dom_node());
}

bool VideoBox::can_have_children() const
{
    // If we allow children when controls are disabled, innerText may be non-empty.
    return dom_node().shadow_root() != nullptr;
}

CSS::SizeWithAspectRatio VideoBox::natural_size() const
{
    CSSPixels width = dom_node().video_width();
    CSSPixels height = dom_node().video_height();
    if (width > 0 && height > 0)
        return { width, height, CSSPixelFraction(width, height) };
    return { width, height, {} };
}

GC::Ptr<Painting::Paintable> VideoBox::create_paintable() const
{
    return Painting::VideoPaintable::create(*this);
}

}
