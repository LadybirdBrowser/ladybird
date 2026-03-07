/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class RadioButtonPaintable final : public PaintableBox {
    GC_CELL(RadioButtonPaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(RadioButtonPaintable);

public:
    static GC::Ref<RadioButtonPaintable> create(Layout::RadioButton const&);

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

private:
    RadioButtonPaintable(Layout::RadioButton const&);
};

}
