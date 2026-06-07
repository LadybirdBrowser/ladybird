/*
 * Copyright (c) 2025-2026, Jonathan Gamble <gamblej@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/Layout/BlockContainer.h>

namespace Web::Layout {

class TextInputBox : public BlockContainer {
    LAYOUT_NODE(TextInputBox, BlockContainer);

public:
    TextInputBox(DOM::Document&, GC::Ptr<DOM::Element>, CSS::ComputedProperties const&);

    HTML::HTMLInputElement const& dom_node() const { return static_cast<HTML::HTMLInputElement const&>(*Box::dom_node()); }
    static CSS::SizeWithAspectRatio auto_content_box_size_for_text_control(HTML::HTMLInputElement const&, Box const&);

    virtual ~TextInputBox() override = default;

private:
    virtual CSS::SizeWithAspectRatio compute_auto_content_box_size() const override;
    virtual bool has_auto_content_box_size() const override { return true; }
};

}
