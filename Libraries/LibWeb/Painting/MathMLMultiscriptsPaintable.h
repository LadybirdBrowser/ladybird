/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/MathMLMultiscriptsBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class MathMLMultiscriptsPaintable final : public PaintableBox {
    GC_CELL(MathMLMultiscriptsPaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(MathMLMultiscriptsPaintable);

public:
    static GC::Ref<MathMLMultiscriptsPaintable> create(Layout::MathMLMultiscriptsBox const&);

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    Layout::MathMLMultiscriptsBox const& layout_box() const;

protected:
    MathMLMultiscriptsPaintable(Layout::MathMLMultiscriptsBox const&);

private:
    virtual bool is_mathml_multiscripts_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<MathMLMultiscriptsPaintable>() const { return is_mathml_multiscripts_paintable(); }

}
