/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ListItemMarkerBox.h>

namespace Web::Layout {

ListItemMarkerBox::ListItemMarkerBox(DOM::Document& document, CSS::ListStyleType style_type, CSS::ListStylePosition style_position, NonnullRefPtr<CSS::ComputedValues const> style)
    : BlockContainer(document, nullptr, style)
    , m_list_style_type(style_type)
    , m_list_style_position(style_position)
{
}

ListItemMarkerBox::~ListItemMarkerBox() = default;

}
