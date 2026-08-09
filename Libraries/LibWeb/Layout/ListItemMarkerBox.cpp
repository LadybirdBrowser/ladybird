/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ListItemMarkerBox.h>

namespace Web::Layout {

ListItemMarkerBox::ListItemMarkerBox(DOM::Document& document, bool is_inside, CSS::LayoutStyle style)
    : BlockContainer(document, nullptr, style)
{
    set_flag(RustFFI::NodeFlag::ListMarkerIsInside, is_inside);
}

ListItemMarkerBox::~ListItemMarkerBox() = default;

}
