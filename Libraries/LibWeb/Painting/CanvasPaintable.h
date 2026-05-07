/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/CanvasBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class CanvasPaintable final : public PaintableBox {
public:
    static NonnullRefPtr<CanvasPaintable> create(Layout::CanvasBox const&);
    virtual StringView class_name() const override { return "CanvasPaintable"sv; }

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

private:
    CanvasPaintable(Layout::CanvasBox const&);
};

}
