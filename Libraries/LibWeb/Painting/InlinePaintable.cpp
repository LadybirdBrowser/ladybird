/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/InlinePaintable.h>

namespace Web::Painting {

NonnullRefPtr<InlinePaintable> InlinePaintable::create(Layout::NodeWithStyle const& layout_node)
{
    return adopt_ref(*new InlinePaintable(layout_node));
}

InlinePaintable::InlinePaintable(Layout::NodeWithStyle const& layout_node)
    : Paintable(layout_node)
{
}

InlinePaintable::~InlinePaintable() = default;

}
