/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

class FrameBox final : public ReplacedBox {
    GC_CELL(FrameBox, ReplacedBox);
    GC_DECLARE_ALLOCATOR(FrameBox);

public:
    FrameBox(DOM::Document&, DOM::Element&, CSS::StyleProperties);
    virtual ~FrameBox() override;

    virtual void prepare_for_replaced_layout() override;

    const HTML::HTMLIFrameElement& dom_node() const { return verify_cast<HTML::HTMLIFrameElement>(ReplacedBox::dom_node()); }
    HTML::HTMLIFrameElement& dom_node() { return verify_cast<HTML::HTMLIFrameElement>(ReplacedBox::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

private:
    virtual void did_set_content_size() override;
};

}
