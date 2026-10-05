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

Box::Box(DOM::Document& document, BindToPreparedArenaSlot bind, Compositing::RustFFI::NodeSlotId slot, RustFFI::NodeKind kind)
    : NodeWithStyle(document, bind, slot, kind)
{
}

Box::~Box()
{
}

static ImageProvider const* image_provider_for_element(DOM::Element const& element)
{
    if (auto const* image = as_if<HTML::HTMLImageElement>(element))
        return image;
    if (auto const* input = as_if<HTML::HTMLInputElement>(element))
        return input;
    if (auto const* object = as_if<HTML::HTMLObjectElement>(element))
        return object;
    return nullptr;
}

ImageProvider* Box::owned_image_provider() const
{
    return static_cast<ImageProvider*>(RustFFI::document_host_owned_image_provider(document_host(), Node::slot_id(this)));
}

ImageProvider const* Box::image_provider_if_any() const
{
    VERIFY(kind() == RustFFI::NodeKind::ImageBox);
    if (auto const* owned = owned_image_provider())
        return owned;
    auto const* element = as_if<DOM::Element>(dom_node());
    return element ? image_provider_for_element(*element) : nullptr;
}

ImageProvider const& Box::image_provider() const
{
    auto const* image_provider = image_provider_if_any();
    VERIFY(image_provider);
    return *image_provider;
}

void Box::set_owned_image_provider(NonnullOwnPtr<ImageProvider> image_provider)
{
    VERIFY(kind() == RustFFI::NodeKind::ImageBox);
    RustFFI::document_host_set_owned_image_provider(document_host(), Node::slot_id(this), image_provider.leak_ptr());
}

bool Box::is_partial_relayout_boundary() const
{
    return RustFFI::render_state_node_is_partial_relayout_boundary(document_host(), Node::slot_id(this));
}

}
