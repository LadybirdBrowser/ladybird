/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/RefPtr.h>
#include <Compositor/Forward.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibMediaCodecs/Color/CodingIndependentCodePoints.h>
#include <LibMediaCodecs/Forward.h>

class SkImage;
class SkYUVAPixmaps;
enum SkYUVColorSpace : int;

template<typename T>
class sk_sp;

namespace Compositor {

SkYUVColorSpace skia_yuv_color_space(Media::CodingIndependentCodePoints);

// Views the planes of the YUV data as Skia pixmaps. Samples deeper than 8 bits are copied and stretched to 16 bits.
SkYUVAPixmaps make_yuva_pixmaps(Gfx::YUVData const&);

// Views a decoder's surface as a GPU image, sampling its planes where they already are instead of uploading a copy.
// Returns null for a surface whose layout is not one the platform decoder produces, or on a platform that cannot
// sample its surfaces yet.
sk_sp<SkImage> sk_image_from_video_surface(Media::VideoSurface const&, Media::CodingIndependentCodePoints, Gfx::SkiaBackendContext&);

// A surface's image stays valid for every frame decoded into the surface, but the decoder hands the surface over afresh
// with each one, which Skia has to be told before it next samples the image.
void video_surface_image_will_show_new_frame(SkImage&);

// Makes an image of the frame's pixels, on the GPU when there is a backend context, and in memory otherwise.
sk_sp<SkImage> sk_image_from_video_frame(Media::VideoFrame const&, RefPtr<Gfx::SkiaBackendContext> const&);

// Makes an image of the frame's pixels in memory, for a canvas that draws there. The CPU converts the frame where it can
// read it, and the GPU, when there is one, reads back a surface that only it can read.
sk_sp<SkImage> sk_image_in_memory_from_video_frame(Media::VideoFrame const&, RefPtr<Gfx::SkiaBackendContext> const&);

// Reads the frame's pixels into memory. A surface is drawn on the GPU and read back from there, since only the GPU
// can read some surfaces.
ErrorOr<Gfx::DecodedImageFrame> decoded_image_frame_from_video_frame(Media::VideoFrame const&, RefPtr<Gfx::SkiaBackendContext> const&);

}
