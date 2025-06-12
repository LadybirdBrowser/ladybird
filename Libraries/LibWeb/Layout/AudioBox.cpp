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

GC::Ptr<Painting::Paintable> AudioBox::create_paintable() const
{
    return Painting::AudioPaintable::create(*this);
}

bool AudioBox::should_paint() const
{
    auto const& audio_element = dom_node();
    return audio_element.has_attribute(HTML::AttributeNames::controls) || audio_element.is_scripting_disabled();
}

void AudioBox::prepare_for_replaced_layout()
{
    if (should_paint()) {
        set_natural_width(300);
        set_natural_height(40);
    } else {
        set_natural_width(0);
        set_natural_height(0);
    }
}

}
