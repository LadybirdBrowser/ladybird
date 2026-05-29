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
    GC_CELL(RangeInputBox, BlockContainer);
    GC_DECLARE_ALLOCATOR(RangeInputBox);

public:
    RangeInputBox(DOM::Document&, GC::Ptr<DOM::Element>, GC::Ref<CSS::ComputedProperties>);

    virtual ~RangeInputBox() override = default;

private:
    virtual CSS::SizeWithAspectRatio compute_auto_content_box_size() const override;
    virtual bool has_auto_content_box_size() const override { return true; }
};

}
