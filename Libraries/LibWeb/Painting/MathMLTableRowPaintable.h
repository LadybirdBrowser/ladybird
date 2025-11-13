/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLTableRowBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class MathMLTableRowPaintable final : public PaintableBox {
    GC_CELL(MathMLTableRowPaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(MathMLTableRowPaintable);

public:
    static GC::Ref<MathMLTableRowPaintable> create(Layout::MathMLTableRowBox const&);

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    Layout::MathMLTableRowBox const& layout_box() const;

protected:
    MathMLTableRowPaintable(Layout::MathMLTableRowBox const&);

private:
    virtual bool is_mathml_table_row_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<MathMLTableRowPaintable>() const { return is_mathml_table_row_paintable(); }

}
