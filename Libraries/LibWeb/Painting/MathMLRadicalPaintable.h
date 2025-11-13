/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLRadicalBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class MathMLRadicalPaintable final : public PaintableBox {
    GC_CELL(MathMLRadicalPaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(MathMLRadicalPaintable);

public:
    static GC::Ref<MathMLRadicalPaintable> create(Layout::MathMLRadicalBox const&);

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    Layout::MathMLRadicalBox const& layout_box() const;

protected:
    MathMLRadicalPaintable(Layout::MathMLRadicalBox const&);

private:
    virtual bool is_mathml_radical_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<MathMLRadicalPaintable>() const { return is_mathml_radical_paintable(); }

}
