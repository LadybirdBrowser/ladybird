/*
 * Copyright (c) 2024, Kostya Farber <kostya.farber@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/FieldSetBox.h>
#include <LibWeb/Painting/PaintContext.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class FieldSetPaintable final : public PaintableBox {
    GC_CELL(FieldSetPaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(FieldSetPaintable);

public:
    static GC::Ref<FieldSetPaintable> create(Layout::FieldSetBox const&);

    virtual void paint(PaintContext&, PaintPhase) const override;

private:
    Layout::FieldSetBox& layout_box();
    Layout::FieldSetBox const& layout_box() const;

    explicit FieldSetPaintable(Layout::FieldSetBox const&);
};

}
