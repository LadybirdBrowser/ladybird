/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Layout {

BlockContainer::BlockContainer(DOM::Document& document, DOM::Node* node, NonnullRefPtr<CSS::StyleProperties> style)
    : Box(document, node, move(style))
{
}

BlockContainer::BlockContainer(DOM::Document& document, DOM::Node* node, NonnullOwnPtr<CSS::ComputedValues> computed_values)
    : Box(document, node, move(computed_values))
{
}

BlockContainer::~BlockContainer() = default;

Painting::PaintableWithLines const* BlockContainer::paintable_with_lines() const
{
    return static_cast<Painting::PaintableWithLines const*>(Box::paintable_box());
}

void BlockContainer::prepare_for_replaced_layout()
{
    if (dom_node() && dom_node()->is_html_input_element()) {
        set_natural_width(CSS::Length(static_cast<HTML::HTMLInputElement*>(dom_node())->size(), CSS::Length::Type::Ch).to_px(*this));
        set_natural_height(CSS::Length(1, CSS::Length::Type::Lh).to_px(*this));
    }
}

JS::GCPtr<Painting::Paintable> BlockContainer::create_paintable() const
{
    return Painting::PaintableWithLines::create(*this);
}

}
