/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/LayoutRustFFI.h>

namespace Web::Painting {

bool push_svg_filter_reference(Layout::BegunRead const&, DOM::Document&, Compositing::RustFFI::NodeSlotId, void const* url_value, void* sink);
void push_svg_paint_server_description(Layout::BegunRead const&, DOM::Document&, Compositing::RustFFI::NodeSlotId, bool is_stroke, void* sink);

}
