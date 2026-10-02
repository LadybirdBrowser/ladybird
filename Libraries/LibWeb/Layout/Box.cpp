/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/Layout/NodeArena.h>

namespace Web::Layout {

Box::Box(DOM::Document& document, GC::Ptr<DOM::Node> node, CSS::LayoutStyle style, RustFFI::NodeKind kind)
    : NodeWithStyle(document, node, move(style), kind)
{
}

Box::Box(DOM::Document& document, BindToPreparedArenaSlot bind, Compositing::RustFFI::NodeSlotId slot, RustFFI::NodeKind kind)
    : NodeWithStyle(document, bind, slot, kind)
{
}

Box::~Box()
{
}

static ImageProvider const& image_provider_for_element(DOM::Element const& element)
{
    if (auto const* image = as_if<HTML::HTMLImageElement>(element))
        return *image;
    if (auto const* input = as_if<HTML::HTMLInputElement>(element))
        return *input;
    if (auto const* object = as_if<HTML::HTMLObjectElement>(element))
        return *object;

    VERIFY_NOT_REACHED();
}

ImageProvider* Box::owned_image_provider() const
{
    return static_cast<ImageProvider*>(RustFFI::layout_arena_owned_image_provider(arena_handle(), Node::slot_id(this)));
}

ImageProvider const& Box::image_provider() const
{
    VERIFY(kind() == RustFFI::NodeKind::ImageBox);
    if (auto const* owned = owned_image_provider())
        return *owned;

    auto const* element = dom_node();
    VERIFY(element);
    return image_provider_for_element(as<DOM::Element>(*element));
}

void Box::set_owned_image_provider(NonnullOwnPtr<ImageProvider> image_provider)
{
    VERIFY(kind() == RustFFI::NodeKind::ImageBox);
    RustFFI::layout_arena_set_owned_image_provider(arena_handle(), Node::slot_id(this), image_provider.leak_ptr());
}

// An element's image provider outlives its box and keeps nothing about it, so only a provider the
// box owns needs to hear about the detach. Detaching thus never needs the element, whose StyleNodeID
// may already be retired.
void Box::notify_owned_image_provider_of_detach()
{
    VERIFY(kind() == RustFFI::NodeKind::ImageBox);
    if (auto* owned = owned_image_provider())
        owned->layout_node_was_detached();
}

bool Box::is_partial_relayout_boundary() const
{
    return RustFFI::layout_arena_node_is_partial_relayout_boundary(arena_handle(), Node::slot_id(this));
}

}
