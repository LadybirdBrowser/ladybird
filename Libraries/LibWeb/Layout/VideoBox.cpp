/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/Layout/VideoBox.h>
#include <LibWeb/Painting/VideoPaintable.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(VideoBox);

VideoBox::VideoBox(DOM::Document& document, DOM::Element& element, GC::Ref<CSS::ComputedProperties> style)
    : ReplacedBox(document, element, style)
{
    document.register_viewport_client(*this);
}

void VideoBox::finalize()
{
    Base::finalize();

    // NOTE: We unregister from the document in finalize() to avoid trouble
    //       in the scenario where our Document has already been swept by GC.
    document().unregister_viewport_client(*this);
}

HTML::HTMLVideoElement& VideoBox::dom_node()
{
    return static_cast<HTML::HTMLVideoElement&>(*ReplacedBox::dom_node());
}

HTML::HTMLVideoElement const& VideoBox::dom_node() const
{
    return static_cast<HTML::HTMLVideoElement const&>(*ReplacedBox::dom_node());
}

CSS::SizeWithAspectRatio VideoBox::natural_size() const
{
    CSSPixels width = dom_node().video_width();
    CSSPixels height = dom_node().video_height();
    if (width > 0 && height > 0)
        return { width, height, CSSPixelFraction(width, height) };
    return { width, height, {} };
}

void VideoBox::did_set_viewport_rect(CSSPixelRect const&)
{
    // FIXME: Several steps in HTMLMediaElement indicate we may optionally handle whether the media object
    //        is in view. Implement those steps.
}

GC::Ptr<Painting::Paintable> VideoBox::create_paintable() const
{
    return Painting::VideoPaintable::create(*this);
}

}
