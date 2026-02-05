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
    GC_CELL(TextInputBox, BlockContainer);
    GC_DECLARE_ALLOCATOR(TextInputBox);

public:
    TextInputBox(DOM::Document&, GC::Ptr<DOM::Element>, GC::Ref<CSS::ComputedProperties>);

    HTML::HTMLInputElement const& dom_node() const { return static_cast<HTML::HTMLInputElement const&>(*Box::dom_node()); }

    virtual ~TextInputBox() override = default;

private:
    virtual CSS::SizeWithAspectRatio compute_auto_content_box_size() const override;
    virtual bool has_auto_content_box_size() const override { return true; }
};

}
