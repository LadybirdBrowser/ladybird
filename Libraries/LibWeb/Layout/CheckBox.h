/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/FormAssociatedLabelableNode.h>

namespace Web::Layout {

class CheckBox final : public FormAssociatedLabelableNode {
    GC_CELL(CheckBox, FormAssociatedLabelableNode);
    GC_DECLARE_ALLOCATOR(CheckBox);

public:
    CheckBox(DOM::Document&, HTML::HTMLInputElement&, CSS::StyleProperties);
    virtual ~CheckBox() override;

private:
    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;
};

}
