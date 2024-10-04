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
    JS_CELL(CanvasBox, ReplacedBox);
    JS_DECLARE_ALLOCATOR(CanvasBox);

public:
    CanvasBox(DOM::Document&, HTML::HTMLCanvasElement&, NonnullRefPtr<CSS::StyleProperties>);
    virtual ~CanvasBox() override;

    virtual void prepare_for_replaced_layout() override;

    const HTML::HTMLCanvasElement& dom_node() const { return static_cast<const HTML::HTMLCanvasElement&>(ReplacedBox::dom_node()); }

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;
};

}
