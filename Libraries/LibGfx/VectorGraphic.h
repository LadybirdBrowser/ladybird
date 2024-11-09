/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>

namespace Gfx {

class VectorGraphic : public RefCounted<VectorGraphic> {
public:
    virtual IntSize intrinsic_size() const = 0;
    virtual void draw_transformed(DeprecatedPainter&, AffineTransform) const = 0;

    IntSize size() const { return intrinsic_size(); }
    IntRect rect() const { return { {}, size() }; }

    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> bitmap(IntSize size, AffineTransform = {}) const;
    void draw_into(DeprecatedPainter&, IntRect const& dest, AffineTransform = {}) const;

    virtual ~VectorGraphic() = default;
};

};
