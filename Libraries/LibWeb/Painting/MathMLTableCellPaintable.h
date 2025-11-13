/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLTableCellBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class MathMLTableCellPaintable final : public PaintableBox {
    GC_CELL(MathMLTableCellPaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(MathMLTableCellPaintable);

public:
    static GC::Ref<MathMLTableCellPaintable> create(Layout::MathMLTableCellBox const&);

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    Layout::MathMLTableCellBox const& layout_box() const;

protected:
    MathMLTableCellPaintable(Layout::MathMLTableCellBox const&);

private:
    virtual bool is_mathml_table_cell_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<MathMLTableCellPaintable>() const { return is_mathml_table_cell_paintable(); }

}
