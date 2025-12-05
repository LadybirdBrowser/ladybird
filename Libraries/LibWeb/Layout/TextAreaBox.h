/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/Layout/BlockContainer.h>

namespace Web::Layout {

class TextAreaBox : public BlockContainer {
    GC_CELL(TextAreaBox, BlockContainer);

public:
    TextAreaBox(DOM::Document&, GC::Ptr<DOM::Element>, GC::Ref<CSS::ComputedProperties>);

    HTML::HTMLTextAreaElement const& dom_node() const { return static_cast<HTML::HTMLTextAreaElement const&>(*Box::dom_node()); }

    virtual ~TextAreaBox() override = default;

protected:
    virtual Optional<CSSPixels> compute_natural_width() const override;
    virtual Optional<CSSPixels> compute_natural_height() const override;

private:
    virtual bool is_textarea_box() const override { return true; }
};

}
