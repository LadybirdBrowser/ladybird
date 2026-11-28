/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Span.h>
#include <CameraServer/CameraBackend.h>

namespace CameraServer {

// Writes the picture as I420 with no padding, which is what the clients read. Fails if the frame is smaller than it
// says, or the destination smaller than the picture.
ErrorOr<void> convert_to_i420(RawFrame const&, Bytes destination);

struct PictureSize {
    u32 width { 0 };
    u32 height { 0 };

    bool operator==(PictureSize const&) const = default;
};

// Clamp the requested size to the camera's size; never upscale. A request for only one dimension preserves aspect ratio.
PictureSize output_size_for(PictureSize camera, Camera::StreamRequest const&);

// Downscale I420 pictures, cropping centrally to the destination aspect ratio.
class PictureScaler {
public:
    static ErrorOr<PictureScaler> create(PictureSize source, PictureSize destination);

    // Both hold a whole picture of their size, with no padding.
    void scale(ReadonlyBytes source, Bytes destination) const;

private:
    PictureSize m_source;
    PictureSize m_destination;
    // The part of the source that is scaled. It starts on even pixels, where a pixel of chroma starts too.
    u32 m_crop_x { 0 };
    u32 m_crop_y { 0 };
    PictureSize m_crop;
};

// Detect CPU capabilities before sandboxing, while the conversion library can read the files it needs.
void prepare_frame_conversion();

}
