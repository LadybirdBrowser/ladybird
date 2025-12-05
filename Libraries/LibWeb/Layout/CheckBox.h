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
    CheckBox(DOM::Document&, HTML::HTMLInputElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~CheckBox() override;

protected:
    virtual Optional<CSSPixels> compute_natural_width() const override { return CSSPixels(13); }
    virtual Optional<CSSPixels> compute_natural_height() const override { return CSSPixels(13); }
    virtual Optional<CSSPixelFraction> compute_natural_aspect_ratio() const override { return {}; }

private:
    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;
};

}
