/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/FormAssociatedLabelableNode.h>

namespace Web::Layout {

class CheckBox final : public FormAssociatedLabelableNode {
    JS_CELL(CheckBox, FormAssociatedLabelableNode);
    JS_DECLARE_ALLOCATOR(CheckBox);

public:
    CheckBox(DOM::Document&, HTML::HTMLInputElement&, NonnullRefPtr<CSS::StyleProperties>);
    virtual ~CheckBox() override;

private:
    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;
};

}
