/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLAudioElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/HTML/HTMLAudioElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/AudioBox.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLAudioElement);

HTMLAudioElement::HTMLAudioElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLMediaElement(document, move(qualified_name))
{
}

HTMLAudioElement::~HTMLAudioElement() = default;

void HTMLAudioElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLAudioElement);
    Base::initialize(realm);
}

GC::Ptr<Layout::Node> HTMLAudioElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::AudioBox>(document(), *this, style);
}

void HTMLAudioElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));
}

Layout::AudioBox* HTMLAudioElement::layout_node()
{
    return static_cast<Layout::AudioBox*>(Node::layout_node());
}

bool HTMLAudioElement::should_paint() const
{
    return has_attribute(HTML::AttributeNames::controls) || is_scripting_disabled();
}

Layout::AudioBox const* HTMLAudioElement::layout_node() const
{
    return static_cast<Layout::AudioBox const*>(Node::layout_node());
}

}
