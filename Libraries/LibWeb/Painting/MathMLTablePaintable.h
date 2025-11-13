/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLTableBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class MathMLTablePaintable final : public PaintableBox {
    GC_CELL(MathMLTablePaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(MathMLTablePaintable);

public:
    static GC::Ref<MathMLTablePaintable> create(Layout::MathMLTableBox const&);

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    Layout::MathMLTableBox const& layout_box() const;

protected:
    MathMLTablePaintable(Layout::MathMLTableBox const&);

private:
    virtual bool is_mathml_table_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<MathMLTablePaintable>() const { return is_mathml_table_paintable(); }

}
