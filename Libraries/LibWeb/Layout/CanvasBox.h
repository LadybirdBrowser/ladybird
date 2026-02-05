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
    GC_CELL(CanvasBox, ReplacedBox);
    GC_DECLARE_ALLOCATOR(CanvasBox);

public:
    CanvasBox(DOM::Document&, HTML::HTMLCanvasElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~CanvasBox() override;

    HTML::HTMLCanvasElement const& dom_node() const { return static_cast<HTML::HTMLCanvasElement const&>(*ReplacedBox::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual CSS::SizeWithAspectRatio compute_auto_content_box_size() const override;
};

}
