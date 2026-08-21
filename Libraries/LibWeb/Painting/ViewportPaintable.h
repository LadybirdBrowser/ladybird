/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Painting/PaintableWithLines.h>

namespace Web::Painting {

class WEB_API ViewportPaintable final : public PaintableWithLines {
public:
    static NonnullRefPtr<ViewportPaintable> create(Layout::Viewport const&);
    virtual ~ViewportPaintable() override;

    virtual void reset_for_relayout() override;

private:
    explicit ViewportPaintable(Layout::Viewport const&);
};

template<>
inline bool Paintable::fast_is<ViewportPaintable>() const { return is_viewport_paintable(); }

}
