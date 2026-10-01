/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/HTMLAudioElement.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/HTML/HTMLAudioElement.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLAudioElement);

HTMLAudioElement::HTMLAudioElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLMediaElement(document, move(qualified_name))
{
}

HTMLAudioElement::~HTMLAudioElement() = default;

CSS::ElementBoxKind HTMLAudioElement::box_kind() const
{
    return CSS::ElementBoxKind::Audio;
}

bool HTMLAudioElement::should_paint() const
{
    return has_attribute(HTML::AttributeNames::controls) || is_scripting_disabled();
}

}
