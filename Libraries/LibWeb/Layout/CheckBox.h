/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

class CheckBox final : public ReplacedBox {
    LAYOUT_NODE(CheckBox, ReplacedBox);

public:
    CheckBox(DOM::Document&, HTML::HTMLInputElement&, CSS::ComputedProperties const&);
    virtual ~CheckBox() override;

private:
    virtual CSS::SizeWithAspectRatio compute_auto_content_box_size() const override { return { 13, 13, {} }; }
    virtual RefPtr<Painting::Paintable> create_paintable() const override;
};

}
