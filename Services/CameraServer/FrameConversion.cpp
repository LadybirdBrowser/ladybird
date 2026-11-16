/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <CameraServer/FrameConversion.h>
#include <libyuv.h>

namespace CameraServer {

void prepare_frame_conversion()
{
    (void)libyuv::TestCpuFlag(0);
}

// The library trusts what it is told of its buffers, so every plane is checked to be all there before it reads one.
static ErrorOr<void> require(ReadonlyBytes data, size_t bytes_per_row, size_t row_bytes, size_t rows)
{
    if (rows == 0)
        return {};
    if (bytes_per_row < row_bytes)
        return Error::from_string_literal("A camera frame's rows are shorter than its picture is wide");
    Checked<size_t> needed = bytes_per_row;
    needed *= rows - 1;
    needed += row_bytes;
    if (needed.has_overflow() || data.size() < needed.value())
        return Error::from_string_literal("A camera frame is smaller than its picture");
    return {};
}

static ReadonlyBytes after(ReadonlyBytes data, size_t size)
{
    return size <= data.size() ? data.slice(size) : ReadonlyBytes {};
}

ErrorOr<void> convert_to_i420(RawFrame const& frame, Bytes destination)
{
    size_t width = frame.width;
    size_t height = frame.height;
    if (width == 0 || height == 0 || width > Camera::MAXIMUM_PICTURE_DIMENSION || height > Camera::MAXIMUM_PICTURE_DIMENSION)
        return Error::from_string_literal("A camera frame has an unusable size");
    if (destination.size() < Camera::i420_size(frame.width, frame.height))
        return Error::from_string_literal("No room for a camera frame");
    // NB: The sizes are small enough for the library's ints; the rows have to be as well.
    if (frame.bytes_per_row > NumericLimits<int>::max() / 2)
        return Error::from_string_literal("A camera frame's rows are too long");

    auto chroma_width = (width + 1) / 2;
    auto chroma_height = (height + 1) / 2;
    auto* y = destination.data();
    auto* u = y + width * height;
    auto* v = u + chroma_width * chroma_height;
    auto const* source = frame.data.data();
    auto bytes_per_row = frame.bytes_per_row;

    auto picture_width = static_cast<int>(width);
    auto picture_height = static_cast<int>(height);
    auto y_stride = static_cast<int>(width);
    auto chroma_stride = static_cast<int>(chroma_width);
    auto source_stride = static_cast<int>(bytes_per_row);

    int result = -1;
    switch (frame.format) {
    case PixelFormat::I420: {
        auto chroma_bytes_per_row = (bytes_per_row + 1) / 2;
        TRY(require(frame.data, bytes_per_row, width, height));
        auto chroma = after(frame.data, bytes_per_row * height);
        TRY(require(chroma, chroma_bytes_per_row, chroma_width, chroma_height * 2));
        result = libyuv::I420Copy(
            source, source_stride,
            chroma.data(), static_cast<int>(chroma_bytes_per_row),
            chroma.data() + chroma_bytes_per_row * chroma_height, static_cast<int>(chroma_bytes_per_row),
            y, y_stride, u, chroma_stride, v, chroma_stride,
            picture_width, picture_height);
        break;
    }
    case PixelFormat::NV12: {
        TRY(require(frame.data, bytes_per_row, width, height));
        auto chroma = after(frame.data, bytes_per_row * height);
        TRY(require(chroma, bytes_per_row, chroma_width * 2, chroma_height));
        result = libyuv::NV12ToI420(
            source, source_stride,
            chroma.data(), source_stride,
            y, y_stride, u, chroma_stride, v, chroma_stride,
            picture_width, picture_height);
        break;
    }
    case PixelFormat::YUY2:
        // Every two pixels are four bytes, and a picture of odd width has its last pixel in a pair of its own.
        TRY(require(frame.data, bytes_per_row, chroma_width * 4, height));
        result = libyuv::YUY2ToI420(
            source, source_stride,
            y, y_stride, u, chroma_stride, v, chroma_stride,
            picture_width, picture_height);
        break;
    }
    if (result != 0)
        return Error::from_string_literal("A camera frame could not be converted");
    return {};
}

PictureSize output_size_for(PictureSize camera, Camera::StreamRequest const& request)
{
    if (camera.width == 0 || camera.height == 0)
        return camera;
    // Never upscale: clamp each requested dimension to the camera's size.
    if (request.width != 0 && request.height != 0)
        return { min(request.width, camera.width), min(request.height, camera.height) };
    auto scaled = [](u32 length, u32 numerator, u32 denominator) {
        return max(1u, static_cast<u32>((static_cast<u64>(length) * numerator + denominator / 2) / denominator));
    };
    if (request.width != 0 && request.width < camera.width)
        return { request.width, scaled(camera.height, request.width, camera.width) };
    if (request.height != 0 && request.height < camera.height)
        return { scaled(camera.width, request.height, camera.height), request.height };
    return camera;
}

ErrorOr<PictureScaler> PictureScaler::create(PictureSize source, PictureSize destination)
{
    auto usable = [](PictureSize size) {
        return size.width != 0 && size.height != 0 && size.width <= Camera::MAXIMUM_PICTURE_DIMENSION && size.height <= Camera::MAXIMUM_PICTURE_DIMENSION;
    };
    if (!usable(source) || !usable(destination))
        return Error::from_string_literal("A picture has an unusable size");
    if (destination.width > source.width || destination.height > source.height)
        return Error::from_string_literal("Pictures are not made larger");

    // Crop centrally to the destination aspect ratio.
    u32 crop_width = source.width;
    u32 crop_height = source.height;
    if (static_cast<u64>(source.width) * destination.height > static_cast<u64>(source.height) * destination.width)
        crop_width = max(destination.width, static_cast<u32>((static_cast<u64>(source.height) * destination.width + destination.height / 2) / destination.height));
    else
        crop_height = max(destination.height, static_cast<u32>((static_cast<u64>(source.width) * destination.height + destination.width / 2) / destination.width));

    PictureScaler scaler;
    scaler.m_source = source;
    scaler.m_destination = destination;
    scaler.m_crop = { min(crop_width, source.width), min(crop_height, source.height) };
    scaler.m_crop_x = ((source.width - scaler.m_crop.width) / 2) & ~1u;
    scaler.m_crop_y = ((source.height - scaler.m_crop.height) / 2) & ~1u;
    return scaler;
}

void PictureScaler::scale(ReadonlyBytes source, Bytes destination) const
{
    VERIFY(source.size() >= Camera::i420_size(m_source.width, m_source.height));
    VERIFY(destination.size() >= Camera::i420_size(m_destination.width, m_destination.height));

    auto half = [](u32 length) { return (length + 1) / 2; };
    auto source_chroma_width = half(m_source.width);
    auto source_chroma_size = static_cast<size_t>(source_chroma_width) * half(m_source.height);
    auto chroma_width = half(m_destination.width);
    auto chroma_size = static_cast<size_t>(chroma_width) * half(m_destination.height);

    // NB: Even crop offsets keep chroma within the source; halving rounds odd source and crop dimensions up.
    auto const* source_y = source.data() + static_cast<size_t>(m_crop_y) * m_source.width + m_crop_x;
    auto const* source_u = source.data() + static_cast<size_t>(m_source.width) * m_source.height + static_cast<size_t>(m_crop_y / 2) * source_chroma_width + m_crop_x / 2;
    auto const* source_v = source_u + source_chroma_size;
    auto* y = destination.data();
    auto* u = y + static_cast<size_t>(m_destination.width) * m_destination.height;
    auto* v = u + chroma_size;

    // Every pixel is the average of what it covers, where it covers at least two pixels each way, and a blend of its
    // neighbors otherwise.
    auto result = libyuv::I420Scale(
        source_y, static_cast<int>(m_source.width),
        source_u, static_cast<int>(source_chroma_width),
        source_v, static_cast<int>(source_chroma_width),
        static_cast<int>(m_crop.width), static_cast<int>(m_crop.height),
        y, static_cast<int>(m_destination.width),
        u, static_cast<int>(chroma_width),
        v, static_cast<int>(chroma_width),
        static_cast<int>(m_destination.width), static_cast<int>(m_destination.height),
        libyuv::kFilterBox);
    VERIFY(result == 0);
}

}
