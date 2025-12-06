/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLAudioElement.h>
#include <LibWeb/Layout/AudioBox.h>
#include <LibWeb/Painting/AudioPaintable.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(AudioBox);

AudioBox::AudioBox(DOM::Document& document, DOM::Element& element, GC::Ref<CSS::ComputedProperties> style)
    : ReplacedBox(document, element, move(style))
{
}

HTML::HTMLAudioElement& AudioBox::dom_node()
{
    return static_cast<HTML::HTMLAudioElement&>(*ReplacedBox::dom_node());
}

HTML::HTMLAudioElement const& AudioBox::dom_node() const
{
    return static_cast<HTML::HTMLAudioElement const&>(*ReplacedBox::dom_node());
}

GC::Ptr<Painting::Paintable> AudioBox::create_paintable() const
{
    return Painting::AudioPaintable::create(*this);
}

Optional<CSSPixels> AudioBox::compute_natural_width() const
{
    return CSSPixels(dom_node().should_paint() ? 300 : 0);
}

Optional<CSSPixels> AudioBox::compute_natural_height() const
{
    return CSSPixels(dom_node().should_paint() ? 40 : 0);
}

}
