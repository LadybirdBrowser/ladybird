/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/TreeBuilderRustFFI.h>

namespace Web::Layout {

RustFFI::FfiLayoutTreeBuildOutcome build_layout_tree(DOM::Node&);
void detach_top_layer_element_layout_subtree(DOM::Element&);

}
