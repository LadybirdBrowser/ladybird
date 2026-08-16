/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/HTMLAudioElement.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/HTML/HTMLAudioElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Box.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLAudioElement);

HTMLAudioElement::HTMLAudioElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLMediaElement(document, move(qualified_name))
{
}

HTMLAudioElement::~HTMLAudioElement() = default;

RefPtr<Layout::Node> HTMLAudioElement::create_layout_node(CSS::LayoutStyle style)
{
    auto audio_box = make_ref_counted<Layout::Box>(document(), *this, style, Layout::RustFFI::NodeKind::AudioBox);
    audio_box->set_replaced_box_can_have_children(shadow_root() != nullptr);
    return audio_box;
}

bool HTMLAudioElement::should_paint() const
{
    return has_attribute(HTML::AttributeNames::controls) || is_scripting_disabled();
}

}
