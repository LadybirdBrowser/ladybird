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

HTMLAudioElement::HTMLAudioElement(DOM::RustMadePrefix prefix)
    : HTMLMediaElement(prefix)
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
