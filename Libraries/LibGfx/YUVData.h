/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Span.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibMedia/Subsampling.h>

class SkYUVAPixmaps;

namespace Gfx {

// A non-owning view of planar YUV data with the metadata needed for conversion to RGB.
// The plane memory's lifetime is guaranteed by the object handing out the view
// (e.g. Media::VideoFrame).
class YUVData final {
public:
    struct PlaneSizes {
        size_t y;
        size_t u;
        size_t v;
        size_t total;
    };

    static ErrorOr<PlaneSizes> plane_sizes(IntSize size, u8 bit_depth, Media::Subsampling);
    static ErrorOr<YUVData> create(IntSize size, u8 bit_depth, Media::Subsampling, Media::CodingIndependentCodePoints, Bytes y_data, Bytes u_data, Bytes v_data);

    IntSize size() const { return m_size; }
    u8 bit_depth() const { return m_bit_depth; }
    Media::Subsampling subsampling() const { return m_subsampling; }
    Media::CodingIndependentCodePoints const& cicp() const { return m_cicp; }

    // Writable access for decoder to fill the planes after creation
    Bytes y_data() { return m_y_data; }
    Bytes u_data() { return m_u_data; }
    Bytes v_data() { return m_v_data; }
    ReadonlyBytes y_data() const { return m_y_data; }
    ReadonlyBytes u_data() const { return m_u_data; }
    ReadonlyBytes v_data() const { return m_v_data; }

    ErrorOr<NonnullRefPtr<Bitmap>> to_bitmap() const;

    SkYUVAPixmaps make_pixmaps() const;

private:
    YUVData(IntSize size, u8 bit_depth, Media::Subsampling, Media::CodingIndependentCodePoints, Bytes y_data, Bytes u_data, Bytes v_data);

    IntSize m_size;
    u8 m_bit_depth { 0 };
    Media::Subsampling m_subsampling;
    Media::CodingIndependentCodePoints m_cicp;

    Bytes m_y_data;
    Bytes m_u_data;
    Bytes m_v_data;
};

}
