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
    JS_CELL(RadioButton, FormAssociatedLabelableNode);
    JS_DECLARE_ALLOCATOR(RadioButton);

public:
    RadioButton(DOM::Document&, HTML::HTMLInputElement&, NonnullRefPtr<CSS::StyleProperties>);
    virtual ~RadioButton() override;

private:
    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;
};

}
