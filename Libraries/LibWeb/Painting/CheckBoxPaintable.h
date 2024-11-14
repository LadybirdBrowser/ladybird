/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Painting/LabelablePaintable.h>

namespace Web::Painting {

class CheckBoxPaintable final : public LabelablePaintable {
    GC_CELL(CheckBoxPaintable, LabelablePaintable);
    GC_DECLARE_ALLOCATOR(CheckBoxPaintable);

public:
    static GC::Ref<CheckBoxPaintable> create(Layout::CheckBox const&);

    Layout::CheckBox const& layout_box() const;
    Layout::CheckBox& layout_box();

    virtual void paint(PaintContext&, PaintPhase) const override;

private:
    CheckBoxPaintable(Layout::CheckBox const&);
};

}
