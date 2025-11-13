/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLFractionBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class MathMLFractionPaintable final : public PaintableBox {
    GC_CELL(MathMLFractionPaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(MathMLFractionPaintable);

public:
    static GC::Ref<MathMLFractionPaintable> create(Layout::MathMLFractionBox const&);

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    Layout::MathMLFractionBox const& layout_box() const;

protected:
    MathMLFractionPaintable(Layout::MathMLFractionBox const&);

private:
    virtual bool is_mathml_fraction_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<MathMLFractionPaintable>() const { return is_mathml_fraction_paintable(); }

}
