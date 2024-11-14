/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/FormAssociatedLabelableNode.h>

namespace Web::Layout {

class RadioButton final : public FormAssociatedLabelableNode {
    GC_CELL(RadioButton, FormAssociatedLabelableNode);
    GC_DECLARE_ALLOCATOR(RadioButton);

public:
    RadioButton(DOM::Document&, HTML::HTMLInputElement&, CSS::StyleProperties);
    virtual ~RadioButton() override;

private:
    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;
};

}
