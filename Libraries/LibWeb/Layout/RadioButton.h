/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

class RadioButton final : public ReplacedBox {
    LAYOUT_NODE(RadioButton, ReplacedBox);

public:
    RadioButton(DOM::Document&, HTML::HTMLInputElement&, CSS::ComputedProperties const&);
    virtual ~RadioButton() override;

private:
    CSS::SizeWithAspectRatio compute_auto_content_box_size() const override { return { 12, 12, {} }; }
    virtual RefPtr<Painting::Paintable> create_paintable() const override;
};

}
