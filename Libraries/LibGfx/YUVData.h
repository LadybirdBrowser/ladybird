/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/FixedArray.h>
#include <AK/NonnullOwnPtr.h>
#include <LibGfx/Size.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibMedia/Subsampling.h>

class SkYUVAPixmaps;

namespace Gfx {

namespace Details {

struct YUVDataImpl;

}

// Holds planar YUV data with metadata needed for GPU conversion.
// Uses FixedArray for deterministic buffer sizing.
// Not ref-counted - owned directly by ImmutableBitmap via NonnullOwnPtr.
class YUVData final {
public:
    static ErrorOr<NonnullOwnPtr<YUVData>> create(IntSize size, u8 bit_depth, Media::Subsampling, Media::CodingIndependentCodePoints);

    ~YUVData();

    IntSize size() const;
    u8 bit_depth() const;
    Media::Subsampling subsampling() const;
    Media::CodingIndependentCodePoints const& cicp() const;

    // Writable access for decoder to fill buffers after creation
    Bytes y_data();
    Bytes u_data();
    Bytes v_data();

    SkYUVAPixmaps const& skia_yuva_pixmaps() const;

private:
    explicit YUVData(NonnullOwnPtr<Details::YUVDataImpl>);

    NonnullOwnPtr<Details::YUVDataImpl> m_impl;
};

}
