/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/TreeBuilderRustFFI.h>

namespace Web::Layout {

void detach_top_layer_element_layout_subtree(Layout::BegunRead const&, DOM::Element&);

// What a layout tree build owes the rows it stamped for their images, which the document attaches once the layout
// update the build ran in is over: the images a box's style asks for, and the provider of the image a box shows in place
// of its element's contents or of a pseudo-element's generated content. Each answers whether the box was handed a
// provider whose image is already there, which the update laid the box out without.
bool attach_owed_style_resources(Layout::BegunRead const&, DOM::Document&, Compositing::RustFFI::NodeSlotId, bool owns_content_replacement_image);
bool attach_owed_generated_image(Layout::BegunRead const&, DOM::Document&, Compositing::RustFFI::NodeSlotId, u32 element_style_node, RustFFI::FfiPseudoElement, RustFFI::FfiGeneratedImage);

}
