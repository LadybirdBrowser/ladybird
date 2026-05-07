/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/ListItemMarkerBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class MarkerPaintable final : public PaintableBox {
public:
    static NonnullRefPtr<MarkerPaintable> create(Layout::ListItemMarkerBox const&);
    virtual StringView class_name() const override { return "MarkerPaintable"sv; }

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    Layout::ListItemMarkerBox const& layout_box() const;

private:
    MarkerPaintable(Layout::ListItemMarkerBox const&);
};

}
