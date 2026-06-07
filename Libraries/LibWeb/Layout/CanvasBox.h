/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

class CanvasBox final : public ReplacedBox {
    LAYOUT_NODE(CanvasBox, ReplacedBox);

public:
    CanvasBox(DOM::Document&, HTML::HTMLCanvasElement&, CSS::ComputedProperties const&);
    virtual ~CanvasBox() override;

    HTML::HTMLCanvasElement const& dom_node() const { return static_cast<HTML::HTMLCanvasElement const&>(*ReplacedBox::dom_node()); }

    virtual RefPtr<Painting::Paintable> create_paintable() const override;

private:
    virtual CSS::SizeWithAspectRatio compute_auto_content_box_size() const override;
};

}
