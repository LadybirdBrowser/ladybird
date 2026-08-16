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
    TextInputBox(DOM::Document&, GC::Ptr<DOM::Element>, CSS::LayoutStyle);

    HTML::HTMLInputElement const& dom_node() const { return static_cast<HTML::HTMLInputElement const&>(*Box::dom_node()); }

    virtual ~TextInputBox() override = default;
};

}
