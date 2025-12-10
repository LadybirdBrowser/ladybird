/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/Layout/BlockContainer.h>

namespace Web::Layout {

class TextInputBox : public BlockContainer {
    GC_CELL(TextInputBox, BlockContainer);

public:
    TextInputBox(DOM::Document&, GC::Ptr<DOM::Element>, GC::Ref<CSS::ComputedProperties>);

    HTML::HTMLInputElement const& dom_node() const { return static_cast<HTML::HTMLInputElement const&>(*Box::dom_node()); }

    virtual ~TextInputBox() override = default;

private:
    virtual CSS::SizeWithAspectRatio compute_intrinsic_content_box_size() const override;
    virtual bool has_intrinsic_content_box_size() const override { return true; }
};

}
