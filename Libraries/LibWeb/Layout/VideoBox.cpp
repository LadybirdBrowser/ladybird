/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/Layout/VideoBox.h>
#include <LibWeb/Painting/VideoPaintable.h>

namespace Web::Layout {

VideoBox::VideoBox(DOM::Document& document, DOM::Element& element, CSS::LayoutStyle style)
    : ReplacedBox(document, element, style)
{
    set_flag(RustFFI::NodeFlag::ReplacedBoxCanHaveChildren, element.shadow_root() != nullptr);
}

HTML::HTMLVideoElement& VideoBox::dom_node()
{
    return static_cast<HTML::HTMLVideoElement&>(*ReplacedBox::dom_node());
}

HTML::HTMLVideoElement const& VideoBox::dom_node() const
{
    return static_cast<HTML::HTMLVideoElement const&>(*ReplacedBox::dom_node());
}

}
