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
    CheckBox(DOM::Document&, HTML::HTMLInputElement&, CSS::LayoutStyle);
    virtual ~CheckBox() override;
};

}
