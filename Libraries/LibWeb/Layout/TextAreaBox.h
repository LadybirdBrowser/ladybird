/*
 * Copyright (c) 2025-2026, Jonathan Gamble <gamblej@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/Layout/BlockContainer.h>

namespace Web::Layout {

class TextAreaBox : public BlockContainer {
    GC_CELL(TextAreaBox, BlockContainer);
    GC_DECLARE_ALLOCATOR(TextAreaBox);

public:
    TextAreaBox(DOM::Document&, GC::Ptr<DOM::Element>, GC::Ref<CSS::ComputedProperties>);

    HTML::HTMLTextAreaElement const& dom_node() const { return static_cast<HTML::HTMLTextAreaElement const&>(*Box::dom_node()); }

    virtual ~TextAreaBox() override = default;

private:
    virtual CSS::SizeWithAspectRatio compute_auto_content_box_size() const override;
    virtual bool has_auto_content_box_size() const override { return true; }
    virtual bool is_textarea_box() const override { return true; }
};

}
