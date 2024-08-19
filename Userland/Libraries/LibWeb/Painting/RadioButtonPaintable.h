/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/RadioButton.h>
#include <LibWeb/Painting/LabelablePaintable.h>

namespace Web::Painting {

class RadioButtonPaintable final : public LabelablePaintable {
    JS_CELL(RadioButtonPaintable, LabelablePaintable);
    GC_DECLARE_ALLOCATOR(RadioButtonPaintable);

public:
    static GC::Ref<RadioButtonPaintable> create(Layout::RadioButton const&);

    virtual void paint(PaintContext&, PaintPhase) const override;

private:
    RadioButtonPaintable(Layout::RadioButton const&);
};

}
