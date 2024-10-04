/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/LabelableNode.h>
#include <LibWeb/Painting/LabelablePaintable.h>

namespace Web::Layout {

Painting::LabelablePaintable* LabelableNode::paintable()
{
    return static_cast<Painting::LabelablePaintable*>(ReplacedBox::paintable());
}

Painting::LabelablePaintable const* LabelableNode::paintable() const
{
    return static_cast<Painting::LabelablePaintable const*>(ReplacedBox::paintable());
}

}
