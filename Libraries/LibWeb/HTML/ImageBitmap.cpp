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

[[nodiscard]] static WebIDL::ExceptionOr<NonnullRefPtr<Gfx::Bitmap>> create_bitmap_from_bitmap_data(JS::VM& vm, Gfx::BitmapFormat const format, Gfx::AlphaType const alpha_type, int const width, int const height, size_t const pitch, ByteBuffer data)
{
    TRY(validate_bitmap_layout(format, alpha_type, width, height));
    auto required_size = Checked<size_t> { pitch };
    required_size *= static_cast<size_t>(height);
    if (pitch < Gfx::Bitmap::minimum_pitch(width, format) || required_size.has_overflow() || required_size.value() > data.size())
        return WebIDL::DataCloneError::create("Invalid ImageBitmap pixel buffer"_utf16);

    auto bitmap_data = TRY_OR_THROW_OOM(vm, try_make<ByteBuffer>(move(data)));
    auto* pixels = bitmap_data->data();
    return TRY_OR_THROW_OOM(vm, Gfx::Bitmap::create_wrapper(format, alpha_type, Gfx::IntSize(width, height), pitch, pixels, [bitmap_data = move(bitmap_data)] { }));
}

enum class SerializedColorSpaceKind : u8 {
    SRGB,
    CICP,
    ICCProfile,
};

[[nodiscard]] static WebIDL::ExceptionOr<NonnullRefPtr<Gfx::Bitmap>> bitmap_from_shared_pixels(JS::VM& vm, Vector<Core::AnonymousBuffer> const* shared_buffers, u32 const buffer_index, u64 const offset, Gfx::BitmapFormat const format, Gfx::AlphaType const alpha_type, int const width, int const height)
{
    TRY(validate_bitmap_layout(format, alpha_type, width, height));

    if (!shared_buffers || buffer_index >= shared_buffers->size())
        return WebIDL::DataCloneError::create("Invalid ImageBitmap shared pixels"_utf16);
    auto buffer = (*shared_buffers)[buffer_index];

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
#else
    // NB: Without resize seals, snapshot just this bitmap through an operation that reports truncation instead of
    //     faulting. Copying the entire shared buffer for every bitmap would make batch transfers quadratic.
    auto snapshot = buffer.snapshot(static_cast<size_t>(offset), pixel_size);
    if (snapshot.is_error())
        return WebIDL::DataCloneError::create("Failed to snapshot ImageBitmap shared pixels"_utf16);
    if (offset == 0 && buffer.size() == round_up_to_power_of_two(pixel_size, shared_pixels_alignment))
        return TRY_OR_THROW_OOM(vm, Gfx::Bitmap::create_with_anonymous_buffer(format, alpha_type, snapshot.release_value(), { width, height }));

    // NB: A batch must not retain a separate snapshot descriptor for every bitmap. Copy the independent pixels into
    //     ordinary bitmap storage, releasing each temporary descriptor before receiving the next bitmap.
    auto bitmap = TRY_OR_THROW_OOM(vm, Gfx::Bitmap::create(format, alpha_type, { width, height }));
    snapshot.value().bytes().copy_to({ bitmap->scanline_u8(0), pixel_size });
    return bitmap;
#endif

    // NB: A buffer that holds just this bitmap backs it directly. Passing the bitmap on, as to WebGL or the compositor,
    //     then shares the memory rather than copying it.
    Gfx::IntSize const size { width, height };
    if (offset == 0 && buffer.size() == round_up_to_power_of_two(Gfx::Bitmap::size_in_bytes(pitch, height), shared_pixels_alignment))
        return TRY_OR_THROW_OOM(vm, Gfx::Bitmap::create_with_anonymous_buffer(format, alpha_type, move(buffer), size));

    auto* pixels = buffer.data<u8>() + offset;
    return TRY_OR_THROW_OOM(vm, Gfx::Bitmap::create_wrapper(format, alpha_type, size, pitch, pixels, [buffer = move(buffer)] { }));
}

size_t ImageBitmap::shared_pixels_size(Gfx::Bitmap const& bitmap)
{
    auto size = Gfx::Bitmap::size_in_bytes(Gfx::Bitmap::minimum_pitch(bitmap.width(), bitmap.format()), bitmap.height());
    return round_up_to_power_of_two(size, shared_pixels_alignment);
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

// The bitmap data of a frame, which the serialization steps of Bindings/Rust/src/html/image_bitmap.rs write and their
// deserialization steps read. The bytes of the pixels that C++ hands over borrow from the frame.
struct FramePixels {
    i32 width { 0 };
    i32 height { 0 };
    u64 pitch { 0 };
    i32 format { 0 };
    i32 alpha_type { 0 };
    u8 const* data { nullptr };
    size_t data_size { 0 };
    u8 color_space_kind { 0 };
    u8 color_primaries { 0 };
    u8 transfer_characteristics { 0 };
    u8 const* icc_profile { nullptr };
    size_t icc_profile_size { 0 };
};

// What Bindings/Rust/src/html/image_bitmap.rs asks of C++, which owns the encoding of the Gfx::Bitmap of a transferred
// frame: a frame that it is handed over is a Gfx::DecodedImageFrame allocated with new, or null for none.
size_t web_image_bitmap_frame_size_in_bytes(Gfx::DecodedImageFrame const*);
void web_image_bitmap_frame_pixels(Gfx::DecodedImageFrame const*, FramePixels*);
JSCompletion web_image_bitmap_create_frame(void* realm, FramePixels const*, Gfx::DecodedImageFrame** out_frame);
bool web_image_bitmap_copy_to_shared_pixels(ImageBitmapSharedPixels*, Gfx::DecodedImageFrame const*, u32* buffer_index, u64* offset);
JSCompletion web_image_bitmap_create_frame_from_shared_pixels(void* realm, HTML::TransferDataDecoder const*, u32 buffer_index, u64 offset, FramePixels const*, Gfx::DecodedImageFrame** out_frame);

size_t web_image_bitmap_frame_size_in_bytes(Gfx::DecodedImageFrame const* frame)
{
    return frame->bitmap().data_size();
}

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

void web_image_bitmap_frame_pixels(Gfx::DecodedImageFrame const* frame, FramePixels* pixels)
{
    auto const& bitmap = frame->bitmap();
    pixels->width = bitmap.width();
    pixels->height = bitmap.height();
    pixels->pitch = bitmap.pitch();
    pixels->format = to_underlying(bitmap.format());
    pixels->alpha_type = to_underlying(bitmap.alpha_type());
    pixels->data = bitmap.scanline_u8(0);
    pixels->data_size = bitmap.data_size();
    frame->color_space().data().visit(
        [&](Empty) {
            pixels->color_space_kind = to_underlying(SerializedColorSpaceKind::SRGB);
        },
        [&](Gfx::ColorSpace::CICP const& cicp) {
            pixels->color_space_kind = to_underlying(SerializedColorSpaceKind::CICP);
            pixels->color_primaries = to_underlying(cicp.color_primaries);
            pixels->transfer_characteristics = to_underlying(cicp.transfer_characteristics);
        },
        [&](NonnullRefPtr<Gfx::ColorSpace::ICCProfile const> const& icc_profile) {
            pixels->color_space_kind = to_underlying(SerializedColorSpaceKind::ICCProfile);
            pixels->icc_profile = icc_profile->bytes().data();
            pixels->icc_profile_size = icc_profile->bytes().size();
        });
}

[[nodiscard]] static ErrorOr<Gfx::ColorSpace> color_space_from_frame_pixels(FramePixels const& pixels)
{
    switch (static_cast<SerializedColorSpaceKind>(pixels.color_space_kind)) {
    case SerializedColorSpaceKind::SRGB:
        return Gfx::ColorSpace {};
    case SerializedColorSpaceKind::CICP: {
        auto color_primaries = static_cast<Media::ColorPrimaries>(pixels.color_primaries);
        auto transfer_characteristics = static_cast<Media::TransferCharacteristics>(pixels.transfer_characteristics);
        return Gfx::ColorSpace::from_cicp({ color_primaries, transfer_characteristics, Media::MatrixCoefficients::Identity, Media::VideoFullRangeFlag::Full });
    }
    case SerializedColorSpaceKind::ICCProfile: {
        auto icc_bytes = TRY(ByteBuffer::copy(pixels.icc_profile, pixels.icc_profile_size));
        return Gfx::ColorSpace::from_icc_bytes(icc_bytes);
    }
    }
    return Error::from_string_literal("Invalid ImageBitmap color space");
}

JSCompletion web_image_bitmap_create_frame(void* realm, FramePixels const* pixels, Gfx::DecodedImageFrame** out_frame)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto frame = [&]() -> WebIDL::ExceptionOr<Gfx::DecodedImageFrame> {
        auto data = TRY_OR_THROW_OOM(js_realm.vm(), ByteBuffer::copy(pixels->data, pixels->data_size));
        auto bitmap = TRY(create_bitmap_from_bitmap_data(js_realm.vm(), static_cast<Gfx::BitmapFormat>(pixels->format), static_cast<Gfx::AlphaType>(pixels->alpha_type), pixels->width, pixels->height, pixels->pitch, move(data)));
        auto color_space = color_space_from_frame_pixels(*pixels);
        if (color_space.is_error())
            return data_clone_error_from_serialization_error(js_realm, color_space.release_error());
        return Gfx::DecodedImageFrame { *bitmap, color_space.release_value() };
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
    auto const& bitmap = frame->bitmap();
    if (!shared_pixels || ImageBitmap::shared_pixels_size(bitmap) > shared_pixels->buffer.size() - shared_pixels->used_size)
        return false;
    auto const pitch = Gfx::Bitmap::minimum_pitch(bitmap.width(), bitmap.format());
    auto* pixels = shared_pixels->buffer.data<u8>() + shared_pixels->used_size;
    for (int y = 0; y < bitmap.height(); ++y)
        memcpy(pixels + y * pitch, bitmap.scanline_u8(y), pitch);
    *buffer_index = shared_pixels->buffer_index;
    *offset = shared_pixels->used_size;
    shared_pixels->used_size += ImageBitmap::shared_pixels_size(bitmap);
    return true;
}

// AD-HOC: A frame over the pixels at the offset of the shared memory of a transfer, as the transfer steps of
//         Bindings/Rust/src/html/image_bitmap.rs place them there. The values of the bitmap and the color space are in
//         the pixels, whose bytes are unused.
JSCompletion web_image_bitmap_create_frame_from_shared_pixels(void* realm, HTML::TransferDataDecoder const* data_holder, u32 buffer_index, u64 offset, FramePixels const* pixels, Gfx::DecodedImageFrame** out_frame)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto frame = [&]() -> WebIDL::ExceptionOr<Gfx::DecodedImageFrame> {
        auto bitmap = TRY(bitmap_from_shared_pixels(js_realm.vm(), data_holder->shared_buffers(), buffer_index, offset, static_cast<Gfx::BitmapFormat>(pixels->format), static_cast<Gfx::AlphaType>(pixels->alpha_type), pixels->width, pixels->height));
        auto color_space = color_space_from_frame_pixels(*pixels);
        if (color_space.is_error())
            return data_clone_error_from_serialization_error(js_realm, color_space.release_error());
        return Gfx::DecodedImageFrame { *bitmap, color_space.release_value() };
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
    if (bitmap)
        frame = Gfx::DecodedImageFrame { bitmap.release_nonnull() };
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

Gfx::Bitmap const* ImageBitmap::bitmap() const
{
    if (auto const* frame = web_image_bitmap_frame(this))
        return &frame->bitmap();
    return nullptr;
}

}
