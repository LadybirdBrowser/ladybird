/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLAudioElement.h>
#include <LibWeb/Layout/AudioBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(AudioBox);

AudioBox::AudioBox(DOM::Document& document, DOM::Element& element, GC::Ref<CSS::ComputedProperties> style)
    : ReplacedBox(document, element, style)
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

bool AudioBox::can_have_children() const
{
    // If we allow children when controls are disabled, innerText may be non-empty.
    return dom_node().shadow_root() != nullptr;
}

GC::Ptr<Painting::Paintable> AudioBox::create_paintable() const
{
    return Painting::PaintableBox::create(*this);
}

}
