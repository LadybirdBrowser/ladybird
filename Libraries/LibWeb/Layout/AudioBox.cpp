/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLAudioElement.h>
#include <LibWeb/Layout/AudioBox.h>
#include <LibWeb/Painting/AudioPaintable.h>

namespace Web::Layout {

JS_DEFINE_ALLOCATOR(AudioBox);

AudioBox::AudioBox(DOM::Document& document, DOM::Element& element, CSS::StyleProperties style)
    : ReplacedBox(document, element, move(style))
{
    set_natural_width(300);
    set_natural_height(40);
}

HTML::HTMLAudioElement& AudioBox::dom_node()
{
    return static_cast<HTML::HTMLAudioElement&>(ReplacedBox::dom_node());
}

HTML::HTMLAudioElement const& AudioBox::dom_node() const
{
    return static_cast<HTML::HTMLAudioElement const&>(ReplacedBox::dom_node());
}

JS::GCPtr<Painting::Paintable> AudioBox::create_paintable() const
{
    return Painting::AudioPaintable::create(*this);
}

}
