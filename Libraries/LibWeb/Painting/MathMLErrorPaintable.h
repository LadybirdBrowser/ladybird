/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLErrorBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class MathMLErrorPaintable final : public PaintableBox {
    GC_CELL(MathMLErrorPaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(MathMLErrorPaintable);

public:
    static GC::Ref<MathMLErrorPaintable> create(Layout::MathMLErrorBox const&);

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    Layout::MathMLErrorBox const& layout_box() const;

protected:
    MathMLErrorPaintable(Layout::MathMLErrorBox const&);

private:
    virtual bool is_mathml_error_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<MathMLErrorPaintable>() const { return is_mathml_error_paintable(); }

}
