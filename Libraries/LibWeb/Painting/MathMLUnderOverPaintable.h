/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLUnderOverBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class MathMLUnderOverPaintable final : public PaintableBox {
    GC_CELL(MathMLUnderOverPaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(MathMLUnderOverPaintable);

public:
    static GC::Ref<MathMLUnderOverPaintable> create(Layout::MathMLUnderOverBox const&);

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    Layout::MathMLUnderOverBox const& layout_box() const;

protected:
    MathMLUnderOverPaintable(Layout::MathMLUnderOverBox const&);

private:
    virtual bool is_mathml_underover_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<MathMLUnderOverPaintable>() const { return is_mathml_underover_paintable(); }

}
