/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLPhantomBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class MathMLPhantomPaintable final : public PaintableBox {
    GC_CELL(MathMLPhantomPaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(MathMLPhantomPaintable);

public:
    static GC::Ref<MathMLPhantomPaintable> create(Layout::MathMLPhantomBox const&);

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    Layout::MathMLPhantomBox const& layout_box() const;

protected:
    MathMLPhantomPaintable(Layout::MathMLPhantomBox const&);

private:
    virtual bool is_mathml_phantom_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<MathMLPhantomPaintable>() const { return is_mathml_phantom_paintable(); }

}
