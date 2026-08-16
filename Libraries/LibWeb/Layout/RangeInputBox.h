/*
 * Copyright (c) 2026, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/BlockContainer.h>

namespace Web::Layout {

class RangeInputBox final : public BlockContainer {
    LAYOUT_NODE(RangeInputBox, BlockContainer);

public:
    RangeInputBox(DOM::Document&, GC::Ptr<DOM::Element>, CSS::LayoutStyle);

    virtual ~RangeInputBox() override = default;
};

}
