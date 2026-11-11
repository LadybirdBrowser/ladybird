/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/NonnullOwnPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#if defined(AK_OS_LINUX)
#    include <fcntl.h>
#endif

namespace Web::HTML {

static constexpr size_t shared_pixels_alignment = 64;

[[nodiscard]] static WebIDL::ExceptionOr<void> validate_bitmap_layout(Gfx::BitmapFormat const format, Gfx::AlphaType const alpha_type, int const width, int const height)
{
    if (!Gfx::is_valid_bitmap_format(to_underlying(format)) || !Gfx::is_valid_alpha_type(to_underlying(alpha_type)))
        return WebIDL::DataCloneError::create("Invalid ImageBitmap pixel format"_utf16);
    Gfx::IntSize const size { width, height };
    if (size.is_empty() || Gfx::Bitmap::size_would_overflow(format, size))
        return WebIDL::DataCloneError::create("Invalid ImageBitmap dimensions"_utf16);
    return {};
}

[[nodiscard]] static WebIDL::ExceptionOr<NonnullRefPtr<Gfx::Bitmap const>> bitmap_borrowing_bitmap_data(JS::VM& vm, Gfx::BitmapFormat const format, Gfx::AlphaType const alpha_type, int const width, int const height, size_t const pitch, ReadonlyBytes data)
{
    TRY(validate_bitmap_layout(format, alpha_type, width, height));
    auto required_size = Checked<size_t> { pitch };
    required_size *= static_cast<size_t>(height);
    if (pitch < Gfx::Bitmap::minimum_pitch(width, format) || required_size.has_overflow() || required_size.value() > data.size())
        return WebIDL::DataCloneError::create("Invalid ImageBitmap pixel buffer"_utf16);

    NonnullRefPtr<Gfx::Bitmap const> bitmap = TRY_OR_THROW_OOM(vm, Gfx::Bitmap::create_wrapper(format, alpha_type, Gfx::IntSize(width, height), pitch, const_cast<u8*>(data.data())));
    return bitmap;
}

enum class SerializedColorSpaceKind : u8 {
    SRGB,
    CICP,
    ICCProfile,
};

struct SharedPixelsOfFrame {
    Core::AnonymousBuffer shared_memory;
    size_t offset_of_pixels { 0 };
};

[[nodiscard]] static WebIDL::ExceptionOr<SharedPixelsOfFrame> shared_pixels_of_frame(ReceivedImageBitmapSharedPixels* received_shared_pixels, u32 const buffer_index, u64 const offset, Gfx::BitmapFormat const format, Gfx::AlphaType const alpha_type, int const width, int const height)
{
    TRY(validate_bitmap_layout(format, alpha_type, width, height));

    if (!received_shared_pixels || buffer_index >= received_shared_pixels->buffers.size())
        return WebIDL::DataCloneError::create("Invalid ImageBitmap shared pixels"_utf16);
    auto buffer = received_shared_pixels->buffers[buffer_index];

    auto const pitch = Gfx::Bitmap::minimum_pitch(width, format);
    auto const pixel_size = Gfx::Bitmap::size_in_bytes(pitch, height);
    Checked<u64> end = offset;
    end += pixel_size;
    if (offset % alignof(u32) != 0 || end.has_overflow() || end.value() > buffer.size())
        return WebIDL::DataCloneError::create("Invalid ImageBitmap shared pixels"_utf16);

    // NB: Validate resize protection before checking the backing size. A native sender can retain its descriptor
    //     and truncate an unsealed buffer after the size check, faulting any process that accesses its pixels.
#if defined(AK_OS_LINUX)
    auto const seals = fcntl(buffer.fd(), F_GET_SEALS);
    if (seals < 0 || (seals & (F_SEAL_SHRINK | F_SEAL_GROW)) != (F_SEAL_SHRINK | F_SEAL_GROW))
        return WebIDL::DataCloneError::create("Unsealed ImageBitmap shared pixels"_utf16);
    if (buffer.validate_backing_size().is_error())
        return WebIDL::DataCloneError::create("Invalid ImageBitmap shared pixels"_utf16);
    return SharedPixelsOfFrame { move(buffer), static_cast<size_t>(offset) };
#else
    // NB: Without resize seals, snapshot the buffer through an operation that reports truncation instead of faulting.
    //     Every bitmap in the buffer reads from the one snapshot. Copying the entire shared buffer for every bitmap
    //     would make batch transfers quadratic, and a snapshot of each bitmap would keep a descriptor for each.
    auto& snapshot = received_shared_pixels->snapshots_of_buffers_without_resize_seals[buffer_index];
    if (!snapshot.has_value()) {
        auto snapshot_or_error = buffer.snapshot();
        if (snapshot_or_error.is_error())
            return WebIDL::DataCloneError::create("Failed to snapshot ImageBitmap shared pixels"_utf16);
        snapshot = snapshot_or_error.release_value();
    }
    return SharedPixelsOfFrame { *snapshot, static_cast<size_t>(offset) };
#endif
}

size_t ImageBitmap::shared_pixels_size(Gfx::DecodedImageFrame const& frame)
{
    auto const tightly_packed_size = Gfx::Bitmap::size_in_bytes(Gfx::Bitmap::minimum_pitch(frame.width(), frame.format()), frame.height());
    return round_up_to_power_of_two(tightly_packed_size, shared_pixels_alignment);
}

// Hands a decoded frame over to the Rust implementation, which takes it over, or null for none.
static Gfx::DecodedImageFrame* frame_to_hand_over(Optional<Gfx::DecodedImageFrame> frame)
{
    if (!frame.has_value())
        return nullptr;
    return new Gfx::DecodedImageFrame(frame.release_value());
}

extern "C" {

// Defined in Bindings/Rust/src/html/exports.rs.
void* web_image_bitmap_create(JSVM*, Gfx::DecodedImageFrame const* frame);
u32 web_image_bitmap_width(void const* image_bitmap);
u32 web_image_bitmap_height(void const* image_bitmap);
bool web_image_bitmap_is_detached(void const* image_bitmap);
void web_image_bitmap_close(void* image_bitmap);
bool web_image_bitmap_origin_clean(void const* image_bitmap);
void web_image_bitmap_set_origin_clean(void* image_bitmap, bool origin_clean);
Gfx::DecodedImageFrame const* web_image_bitmap_frame(void const* image_bitmap);

// The color space of a frame, which the serialization steps of Bindings/Rust/src/html/image_bitmap.rs write and their
// deserialization steps read. The bytes of an ICC profile that C++ hands over borrow from the frame.
struct FrameColorSpace {
    u8 kind { 0 };
    u8 color_primaries { 0 };
    u8 transfer_characteristics { 0 };
    u8 const* icc_profile { nullptr };
    size_t icc_profile_size { 0 };
};

// The bitmap data that the deserialization steps of Bindings/Rust/src/html/image_bitmap.rs read, which a frame is
// created from.
struct FramePixels {
    i32 width { 0 };
    i32 height { 0 };
    u64 pitch { 0 };
    i32 format { 0 };
    i32 alpha_type { 0 };
    u8 const* data { nullptr };
    size_t data_size { 0 };
    FrameColorSpace color_space;
};

// What Bindings/Rust/src/html/image_bitmap.rs asks of C++, which owns the encoding of the pixels of a transferred
// frame: a frame that it is handed over is a Gfx::DecodedImageFrame allocated with new, or null for none.
void web_image_bitmap_frame_color_space(Gfx::DecodedImageFrame const*, FrameColorSpace*);
JSCompletion web_image_bitmap_create_frame(void* realm, FramePixels const*, Gfx::DecodedImageFrame** out_frame);
bool web_image_bitmap_copy_to_shared_pixels(ImageBitmapSharedPixels*, Gfx::DecodedImageFrame const*, u32* buffer_index, u64* offset);
JSCompletion web_image_bitmap_create_frame_from_shared_pixels(void* realm, ReceivedImageBitmapSharedPixels*, u32 buffer_index, u64 offset, FramePixels const*, Gfx::DecodedImageFrame** out_frame);

// NB: The Rust side names these values by their underlying values.
static_assert(to_underlying(Gfx::BitmapFormat::BGRx8888) == 1);
static_assert(to_underlying(Gfx::BitmapFormat::BGRA8888) == 2);
static_assert(to_underlying(Gfx::BitmapFormat::RGBx8888) == 3);
static_assert(to_underlying(Gfx::BitmapFormat::RGBA8888) == 4);
static_assert(to_underlying(Gfx::AlphaType::Premultiplied) == 0);
static_assert(to_underlying(Gfx::AlphaType::Unpremultiplied) == 1);
static_assert(to_underlying(SerializedColorSpaceKind::SRGB) == 0);
static_assert(to_underlying(SerializedColorSpaceKind::CICP) == 1);
static_assert(to_underlying(SerializedColorSpaceKind::ICCProfile) == 2);

void web_image_bitmap_frame_color_space(Gfx::DecodedImageFrame const* frame, FrameColorSpace* color_space)
{
    frame->color_space().data().visit(
        [&](Empty) {
            color_space->kind = to_underlying(SerializedColorSpaceKind::SRGB);
        },
        [&](Gfx::ColorSpace::CICP const& cicp) {
            color_space->kind = to_underlying(SerializedColorSpaceKind::CICP);
            color_space->color_primaries = to_underlying(cicp.color_primaries);
            color_space->transfer_characteristics = to_underlying(cicp.transfer_characteristics);
        },
        [&](NonnullRefPtr<Gfx::ColorSpace::ICCProfile const> const& icc_profile) {
            color_space->kind = to_underlying(SerializedColorSpaceKind::ICCProfile);
            color_space->icc_profile = icc_profile->bytes().data();
            color_space->icc_profile_size = icc_profile->bytes().size();
        });
}

[[nodiscard]] static ErrorOr<Gfx::ColorSpace> color_space_from(FrameColorSpace const& color_space)
{
    switch (static_cast<SerializedColorSpaceKind>(color_space.kind)) {
    case SerializedColorSpaceKind::SRGB:
        return Gfx::ColorSpace {};
    case SerializedColorSpaceKind::CICP: {
        auto color_primaries = static_cast<Media::ColorPrimaries>(color_space.color_primaries);
        auto transfer_characteristics = static_cast<Media::TransferCharacteristics>(color_space.transfer_characteristics);
        return Gfx::ColorSpace::from_cicp({ color_primaries, transfer_characteristics, Media::MatrixCoefficients::Identity, Media::VideoFullRangeFlag::Full });
    }
    case SerializedColorSpaceKind::ICCProfile: {
        auto icc_bytes = TRY(ByteBuffer::copy(color_space.icc_profile, color_space.icc_profile_size));
        return Gfx::ColorSpace::from_icc_bytes(icc_bytes);
    }
    }
    return Error::from_string_literal("Invalid ImageBitmap color space");
}

JSCompletion web_image_bitmap_create_frame(void* realm, FramePixels const* pixels, Gfx::DecodedImageFrame** out_frame)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto frame = [&]() -> WebIDL::ExceptionOr<Gfx::DecodedImageFrame> {
        auto bitmap = TRY(bitmap_borrowing_bitmap_data(js_realm.vm(), static_cast<Gfx::BitmapFormat>(pixels->format), static_cast<Gfx::AlphaType>(pixels->alpha_type), pixels->width, pixels->height, pixels->pitch, { pixels->data, pixels->data_size }));
        auto color_space = color_space_from(pixels->color_space);
        if (color_space.is_error())
            return data_clone_error_from_serialization_error(js_realm, color_space.release_error());
        auto frame = Gfx::DecodedImageFrame::create(*bitmap, color_space.release_value());
        if (frame.is_error())
            return data_clone_error_from_serialization_error(js_realm, frame.release_error());
        return frame.release_value();
    }();
    if (frame.is_exception())
        return Bindings::completion_to_abi(js_realm, frame.release_error());
    *out_frame = frame_to_hand_over(frame.release_value());
    return Bindings::completion_to_abi(js_realm, JS::js_undefined());
}

// AD-HOC: Copies the pixels of the frame to the shared memory of a transfer, if there is room for them there, and
//         returns where they are.
bool web_image_bitmap_copy_to_shared_pixels(ImageBitmapSharedPixels* shared_pixels, Gfx::DecodedImageFrame const* frame, u32* buffer_index, u64* offset)
{
    if (!shared_pixels || ImageBitmap::shared_pixels_size(*frame) > shared_pixels->buffer.size() - shared_pixels->used_size)
        return false;
    // NB: The receiver reads the rows as tightly packed, which the rows of the frame need not be.
    auto const row_size = Gfx::Bitmap::minimum_pitch(frame->width(), frame->format());
    auto const pixels = frame->pixels();
    auto* destination = shared_pixels->buffer.data<u8>() + shared_pixels->used_size;
    for (int y = 0; y < frame->height(); ++y)
        __builtin_memcpy(destination + static_cast<size_t>(y) * row_size, pixels.offset(static_cast<size_t>(y) * frame->pitch()), row_size);
    *buffer_index = shared_pixels->buffer_index;
    *offset = shared_pixels->used_size;
    shared_pixels->used_size += ImageBitmap::shared_pixels_size(*frame);
    return true;
}

// AD-HOC: A frame over the pixels at the offset of the shared memory of a transfer, as the transfer steps of
//         Bindings/Rust/src/html/image_bitmap.rs place them there. The values of the bitmap and the color space are in
//         the pixels, whose bytes are unused.
JSCompletion web_image_bitmap_create_frame_from_shared_pixels(void* realm, ReceivedImageBitmapSharedPixels* received_shared_pixels, u32 buffer_index, u64 offset, FramePixels const* pixels, Gfx::DecodedImageFrame** out_frame)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto frame = [&]() -> WebIDL::ExceptionOr<Gfx::DecodedImageFrame> {
        auto format = static_cast<Gfx::BitmapFormat>(pixels->format);
        auto alpha_type = static_cast<Gfx::AlphaType>(pixels->alpha_type);
        auto shared_pixels = TRY(shared_pixels_of_frame(received_shared_pixels, buffer_index, offset, format, alpha_type, pixels->width, pixels->height));
        auto color_space = color_space_from(pixels->color_space);
        if (color_space.is_error())
            return data_clone_error_from_serialization_error(js_realm, color_space.release_error());
        auto frame = Gfx::DecodedImageFrame::create_over_shared_memory(move(shared_pixels.shared_memory), shared_pixels.offset_of_pixels, { pixels->width, pixels->height }, format, alpha_type, color_space.release_value());
        if (frame.is_error())
            return data_clone_error_from_serialization_error(js_realm, frame.release_error());
        return frame.release_value();
    }();
    if (frame.is_exception())
        return Bindings::completion_to_abi(js_realm, frame.release_error());
    *out_frame = frame_to_hand_over(frame.release_value());
    return Bindings::completion_to_abi(js_realm, JS::js_undefined());
}
}

GC::Ref<ImageBitmap> ImageBitmap::create(RefPtr<Gfx::Bitmap const> bitmap)
{
    Optional<Gfx::DecodedImageFrame> frame;
    if (bitmap) {
        if (auto frame_or_error = Gfx::DecodedImageFrame::create(*bitmap); !frame_or_error.is_error())
            frame = frame_or_error.release_value();
    }
    return create(move(frame));
}

GC::Ref<ImageBitmap> ImageBitmap::create(Optional<Gfx::DecodedImageFrame> frame)
{
    auto* image_bitmap = web_image_bitmap_create(JS::HostABI::vm_to_abi(JS::VM::the()), frame.has_value() ? &frame.value() : nullptr);
    return GC::Ref { *static_cast<ImageBitmap*>(image_bitmap) };
}

WebIDL::UnsignedLong ImageBitmap::width() const
{
    return web_image_bitmap_width(this);
}

WebIDL::UnsignedLong ImageBitmap::height() const
{
    return web_image_bitmap_height(this);
}

bool ImageBitmap::is_detached() const
{
    return web_image_bitmap_is_detached(this);
}

void ImageBitmap::close()
{
    web_image_bitmap_close(this);
}

bool ImageBitmap::is_origin_clean() const
{
    return web_image_bitmap_origin_clean(this);
}

void ImageBitmap::set_origin_clean(bool origin_clean)
{
    web_image_bitmap_set_origin_clean(this, origin_clean);
}

Gfx::DecodedImageFrame const* ImageBitmap::frame() const
{
    return web_image_bitmap_frame(this);
}

}
