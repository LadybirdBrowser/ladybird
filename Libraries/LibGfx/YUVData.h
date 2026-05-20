/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/FixedArray.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <LibGfx/Forward.h>
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
// Not ref-counted - owned directly by decoded video frame objects via NonnullOwnPtr.
class YUVData final {
public:
    struct PlaneSizes {
        size_t y;
        size_t u;
        size_t v;
        size_t total;
    };

    static ErrorOr<PlaneSizes> plane_sizes(IntSize size, u8 bit_depth, Media::Subsampling);
    static ErrorOr<NonnullOwnPtr<YUVData>> create(IntSize size, u8 bit_depth, Media::Subsampling, Media::CodingIndependentCodePoints);
    static ErrorOr<NonnullOwnPtr<YUVData>> create_from_data(IntSize size, u8 bit_depth, Media::Subsampling, Media::CodingIndependentCodePoints, ReadonlyBytes y_data, ReadonlyBytes u_data, ReadonlyBytes v_data);

    ~YUVData();

    IntSize size() const;
    u8 bit_depth() const;
    Media::Subsampling subsampling() const;
    Media::CodingIndependentCodePoints const& cicp() const;

    // Writable access for decoder to fill buffers after creation
    Bytes y_data();
    Bytes u_data();
    Bytes v_data();
    ReadonlyBytes y_data() const;
    ReadonlyBytes u_data() const;
    ReadonlyBytes v_data() const;

    ErrorOr<NonnullRefPtr<Bitmap>> to_bitmap() const;

    SkYUVAPixmaps make_pixmaps() const;

private:
    explicit YUVData(NonnullOwnPtr<Details::YUVDataImpl>);

    NonnullOwnPtr<Details::YUVDataImpl> m_impl;
};

}
